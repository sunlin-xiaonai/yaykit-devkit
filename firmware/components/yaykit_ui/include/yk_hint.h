// The hint arrow (docs/design/device-controls.md 2, plan P24): when someone may not know where to press, a coral
// V at the screen's edge points at the side key, nudging 8 px towards it, with one short line in a dark pill
// beside it. It shows for 4 s; yk_hint_hide() (any key press) takes it away at once.
//
// When (the caller asks yk_hint_should() at these moments, and shows it if true):
//   TALK   just added: the first 3 times the screen wakes on Yay's home, "按住这里说话"; never again after 3 turns
//   TAP    a tap on Yay's home that did nothing: "按侧键说话", once per wake
//   APPS   the first time in each extension: "双击：应用和回家"; never again once the app ring was opened
//   BACK   back in Yay from "回到 Yay 说？": "按住这里说话"
// Never while the app ring is open; listening, thinking, replying, installing, counting down and the screen being
// off are the caller's to check.
//
// The counts (turns, ring opened, hints shown, extensions seen) are kept in NVS namespace `yk_hint`, shared by all
// firmware on the device (the design's yk_pref; this namespace until yaykit_board's shared settings take them). Each
// is capped at 3, so the flash is written a handful of times in a device's life. Call under board_lock() for
// show/hide; should/note from any task.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YK_KEY_ANGLE_DEG
/** Where the side key is, seen from the screen's centre: degrees clockwise from 12 o'clock, 30 to a clock hour. 45 =
 *  half past one, measured on the tikpal-r1's board (10-22): the side key is the BOOT switch (GPIO0), 45 degrees from
 *  the PWR key at 12 o'clock; on the board's back it is to the left, so seen from the screen it is to the right. The
 *  screen is not rotated or mirrored (yaykit_board: ROTATE_0, no MADCTL), its top towards PWR. The user's guess had
 *  been about 1 o'clock (30); the prototype drew 3 o'clock (90). The arrow and its pill follow this one number. */
#define YK_KEY_ANGLE_DEG 45
#endif

typedef enum {
    YK_HINT_TALK = 0,
    YK_HINT_TAP,
    YK_HINT_APPS,
    YK_HINT_BACK,
    YK_HINT_KIND_COUNT,
} yk_hint_kind_t;

typedef enum {
    YK_HINT_EV_WAKE = 0,     // the screen woke (TAP may show again)
    YK_HINT_EV_TALKED,       // a turn was spoken (TALK stops after 3)
    YK_HINT_EV_RING_OPENED,  // the app ring opened (yk_apps_open() notes it itself; APPS stops)
    YK_HINT_EV_COUNT,
} yk_hint_event_t;

/** Loads the counts; `slug` is this firmware's ("yay", "breathe"...: which extension APPS is about). Without it the
 *  counts load on first use and APPS never shows. */
esp_err_t yk_hint_init(const char *slug);
/** Whether the hint of this kind should show now (the rules above). */
bool yk_hint_should(yk_hint_kind_t kind);
/** Something that changes the rules happened. */
void yk_hint_note(yk_hint_event_t event);

/** Shows the arrow and the pill over `parent` (the active screen) for 4 s, with `text`, or the kind's own line in
 *  the language (yk_ui_set_english) when NULL; counts it as shown. Replaces a hint already showing. */
bool yk_hint_show(lv_obj_t *parent, yk_hint_kind_t kind, const char *text);
/** Takes it away at once. */
void yk_hint_hide(void);
bool yk_hint_is_shown(void);

/** Development: forgets the counts (`st hint reset`). */
void yk_hint_reset(void);

#ifdef __cplusplus
}
#endif

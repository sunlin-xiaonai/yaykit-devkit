// Pure C parts of the app ring, the app catalog and the hint arrow (host tested with test/run.sh, LVGL stubbed):
// where everything on the ring goes and how it moves, what a touch hits, the catalog's bytes on flash, icons to
// RGB565, the hint's geometry and when it may show.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "yk_apps.h"
#include "yk_catalog.h"
#include "yk_hint.h"
#include "yk_ui_logic.h"

// ---------------------------------------------------------------- the ring's geometry (device-controls.md 4)

#define YK_APPS_R          142  // the apps' centres from the screen's centre
#define YK_APPS_SAT_R      38   // an app's disc
#define YK_APPS_CENTER_R   56   // Yay's disc
#define YK_APPS_RING_W     3    // the sage ring of the app in use
#define YK_APPS_YAY        100  // Yay's size in the middle
#define YK_APPS_YAY_DY     2    // ... a little below the disc's centre
#define YK_APPS_NAME_SIZE  16   // the names: the round-screen spec's smallest (the prototype's 15 is below it)
#define YK_APPS_FIRST_SIZE 24   // the first character standing in for a missing icon
#define YK_APPS_NAME_GAP   4    // between a disc and its name
#define YK_APPS_NAME_MAX_W 96   // a longer name is cut with "…" (the lower two: yk_apps_name_room())
#define YK_APPS_NAME_APART 12   // the lower two names keep this far apart
#define YK_APPS_LABEL_DY   62   // the top of the middle's label, below the centre (its disc ends at 56)
#define YK_APPS_LABEL_MAX_W 96  // "回到 Yay" is about 61 px at 16
#define YK_APPS_INK_TOP    4    // a 16 px line's box is empty this far down from its top (Noto Sans SC) ...
#define YK_APPS_INK_BOTTOM 1    // ... and this far up from its bottom (Latin descenders reach lower than Han)
#define YK_APPS_BADGE_D    27   // the download mark's centre, right and down from the app's
#define YK_APPS_BADGE_R    12
#define YK_APPS_HIT_R      48   // an app's touch target: 96 across (YK_TOUCH_MIN_W)
#define YK_APPS_CENTER_HIT_R 60
#define YK_APPS_DIM_OPA    107  // 42 % of 255: an app not installed
#define YK_APPS_CACHE_D    29   // the cached mark's centre, right and down from the app's (the design's 76 px disc)
#define YK_APPS_CACHE_R    7    // ... its outer radius, a hollow ring ...
#define YK_APPS_CACHE_W    3    // ... this wide, filled with paper
#define YK_APPS_MORE_PLACE 4    // 「更多」 stands at the fifth place, upper left (288 degrees)
#define YK_APPS_DOT_R      5    // 「更多」's three dots ...
#define YK_APPS_DOT_GAP    14   // ... this far apart
// The ring with 「设置」 (settings-u.md 5, BOSS-2 10-25): 6 places every 60 degrees from 12 o'clock on r 128 (at r 142
// the name under the bottom one would leave the safe circle). Its places are numbered YK_APPS_SIX + 0..5, apart from
// the 5-place ring's 0..4.
#define YK_APPS_SIX        8
#define YK_APPS_R6         128
#define YK_APPS_MORE_PLACE6     (YK_APPS_SIX + 4)  // 「更多」: lower left (240 degrees)
#define YK_APPS_SETTINGS_PLACE  (YK_APPS_SIX + 5)  // 「设置」: upper left (300 degrees)
#define YK_APPS_GEAR_UNIT  1.5f // 「设置」's gear: the canvas's 24-unit icon at 36 px

typedef struct {
    int16_t x, y, w, h;
} yk_box_t;

typedef struct {
    int slot;           // 0..4, clockwise from 12 o'clock, every 72 degrees; YK_APPS_SIX + 0..5 every 60 (r 128)
    int16_t cx, cy;     // the disc's centre, screen coordinates
    yk_box_t name;      // the name's line box, under the disc
    int16_t name_shift; // px the name moved towards the middle to keep its glyphs in the content circle
} yk_apps_sat_t;

typedef struct {
    int n;
    yk_apps_sat_t sat[YK_APPS_MAX];
    yk_box_t label;     // the middle's label
} yk_apps_layout_t;

/** Which of the 5 places n apps take: all for 5; fewer stand symmetrically (1: the top; 2: upper right then upper
 *  left; 3: top, lower right, lower left; 4: all but the top). */
void yk_apps_slots(int n, int slots[YK_APPS_MAX]);
/** The places of these items: with 「设置」 last (YK_APPS_SETTINGS), the 6-place ring: the apps from the top clockwise,
 *  「更多」 (before it) at YK_APPS_MORE_PLACE6, 「设置」 at YK_APPS_SETTINGS_PLACE; with 「更多」 last (YK_APPS_MORE), the
 *  apps from the top clockwise and 「更多」 at YK_APPS_MORE_PLACE; otherwise yk_apps_slots(n). */
void yk_apps_item_slots(const yk_apps_item_t *items, int n, int slots[YK_APPS_MAX]);
/** The widest name (px, YK_APPS_NAME_MAX_W at most) the place `slot` allows for a line name_h px tall: under its
 *  disc, the glyphs in the content circle, the name's middle within the disc's width, and the lower two each on
 *  their own side of the screen's middle (12 px apart). About 81 for the lower two, 96 for the others. */
int yk_apps_name_room(int slot, int name_h);
/** Lays out n apps whose names are name_w[i] px wide (already cut to their yk_apps_name_room()) and name_h px tall (the
 *  font's line height), and the middle's label label_w px wide. Every name goes under its disc; the lower two
 *  would reach past the content circle (r 200) there, so they move sideways towards the middle until their glyphs
 *  (the line box less YK_APPS_INK_TOP / _BOTTOM) are inside it. */
void yk_apps_layout(int n, const int name_w[], int name_h, int label_w, yk_apps_layout_t *out);
/** The same at the places given (yk_apps_item_slots()). */
void yk_apps_layout_at(int n, const int slots[], const int name_w[], int name_h, int label_w, yk_apps_layout_t *out);

// ---------------------------------------------------------------- motion

#define YK_APPS_OPEN_MS      300   // the last app in place
#define YK_APPS_CENTER_IN_MS 240   // the middle grows from 0.6
#define YK_APPS_STAGGER_MS   28    // each app leaves the centre this much after the one before
#define YK_APPS_FLY_MS       (YK_APPS_OPEN_MS - (YK_APPS_MAX - 1) * YK_APPS_STAGGER_MS)  // 188
#define YK_APPS_CLOSE_MS     180
#define YK_APPS_IDLE_MS      8000  // untouched this long: it closes by itself

typedef struct {
    int16_t cx, cy;   // where the element's centre is now
    uint16_t scale;   // 256 = full size
    uint8_t opa;
} yk_apps_pose_t;

/** The ring t_ms after it started opening (closing: after it started closing). pose[0] is the middle, pose[1..n]
 *  the apps. Opening: the middle from 0.6 and clear, the apps from the centre at 0.2 and clear, eased out; closing:
 *  all back towards the centre (the middle to 0.8), eased in. Returns true once the motion is over. */
bool yk_apps_frame(const yk_apps_layout_t *L, int t_ms, bool closing, yk_apps_pose_t pose[YK_APPS_MAX + 1]);

/** What a touch at (x, y) hits: 0 the middle, 1..n an app (its disc or its name), -1 the paper. */
int yk_apps_hit(const yk_apps_layout_t *L, int x, int y);

/** `text` cut to max_w px (glyph widths from `width`), ending in "…" when it was longer. Returns its width. */
int yk_text_fit(const char *text, yk_glyph_width_fn width, void *ctx, int max_w, char *out, size_t size);
/** The first character of a name (what stands in for its icon); "" for none. */
void yk_first_char(const char *name, char out[5]);

// ---------------------------------------------------------------- the catalog's bytes (yk_catalog.h)

#define YK_CATALOG_MAGIC   0x43414B59u  // "YKAC"
#define YK_CATALOG_VERSION 1
#define YK_CATALOG_HEADER  64
#define YK_CATALOG_TABLE_AT 4096        // the table and the icons start in the second sector
#define YK_CATALOG_ENTRY   128

/** CRC-32 (IEEE), continuing from `crc` (0 to start). (yaykit_board has a one-shot yk_crc32 of its own.) */
uint32_t yk_catalog_crc32(uint32_t crc, const void *data, size_t len);
/** The region's bytes for this catalog: what to write at offset 0 (header) and at YK_CATALOG_TABLE_AT (the
 *  table, then the icons). out must hold YK_CATALOG_REGION bytes; returns the bytes used from 0 (0 when it does
 *  not fit or an entry is bad). The icon offsets and CRCs are filled into the copy written. */
size_t yk_catalog_encode(const yk_catalog_t *cat, const uint8_t *const icons[], uint8_t *out, size_t size);
/** The header's fields; false when it is not a catalog this reader knows. *table_len: bytes of the table. */
bool yk_catalog_decode_header(const uint8_t *h, yk_catalog_t *out, uint32_t *table_len, uint32_t *table_crc);
/** The table into out->app (after decode_header); false when its CRC or an entry is wrong. */
bool yk_catalog_decode_table(const uint8_t *t, uint32_t len, uint32_t crc, yk_catalog_t *out);

#define YK_ICON_RGB565A8_BYTES (YK_CATALOG_ICON * YK_CATALOG_ICON * 3)
/** A 64 x 64 catalog icon as LVGL's RGB565A8 (the colours, then the alpha plane): the icon's own alpha times opa
 *  (255: as it is; YK_APPS_DIM_OPA: an app not installed), so it sits on the disc with its corners clear. A8 is
 *  drawn in `color`. */
void yk_icon_rgb565a8(uint8_t format, const uint8_t *src, uint32_t color, uint8_t opa, uint8_t *dst);

// ---------------------------------------------------------------- the hint arrow (device-controls.md 2)

#define YK_HINT_TIP_R     201   // the V's point from the centre (the prototype's 434 - 233)
#define YK_HINT_V_DEPTH   16    // from the point back to the arms' ends
#define YK_HINT_V_HALF    14    // half the V's opening
#define YK_HINT_STROKE    5
#define YK_HINT_NUDGE     8     // towards the key and back
#define YK_HINT_NUDGE_MS  700   // each way
#define YK_HINT_PILL_R    171   // the pill reaches this far towards the key
#define YK_HINT_PILL_H    40
#define YK_HINT_PILL_PAD  16    // left and right of the text
#define YK_HINT_TEXT_SIZE 18    // YK_SIZE_STATUS
#define YK_HINT_SHOW_MS   4000
#define YK_HINT_FADE_MS   200

typedef struct {
    float ux, uy;              // unit vector towards the key (screen: x right, y down)
    float tip_x, tip_y;        // the V: arm, point, arm (screen coordinates, before the nudge)
    float a1_x, a1_y, a2_x, a2_y;
    yk_box_t pill;             // the pill's box
} yk_hint_geo_t;

/** The arrow and the pill for a key at key_deg (clockwise from 12) and a line text_w px wide: the pill as close to
 *  the key as it can be with its corners inside the content circle. */
void yk_hint_geometry(float key_deg, int text_w, yk_hint_geo_t *g);

#define YK_HINT_CAP 3  // counts stop here (the rules only need "fewer than 3")

typedef struct {
    // saved in NVS
    uint8_t talked;      // turns spoken
    uint8_t ring;        // times the app ring opened
    uint8_t talk_shown;  // times TALK showed
    uint32_t seen;       // extensions whose APPS hint showed (yk_hint_app_bit)
    // this boot only
    bool tap_shown;      // TAP showed since the screen woke
    uint32_t app_bit;    // this firmware's bit (0: Yay, or not known)
} yk_hint_state_t;

/** An extension's bit in `seen` (a hash of its slug; never 0); 0 for Yay and NULL. */
uint32_t yk_hint_app_bit(const char *slug);
/** The rules (yk_hint.h) without the "ring open" one. */
bool yk_hint_rule(const yk_hint_state_t *s, yk_hint_kind_t kind);
/** An event; returns true when a saved count changed. */
bool yk_hint_apply(yk_hint_state_t *s, yk_hint_event_t ev);
/** The hint of this kind was shown; returns true when a saved count changed. */
bool yk_hint_shown(yk_hint_state_t *s, yk_hint_kind_t kind);
/** The kind's own line. */
const char *yk_hint_text(yk_hint_kind_t kind, bool english);

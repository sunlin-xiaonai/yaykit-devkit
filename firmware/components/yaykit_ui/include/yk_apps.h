// The app ring (docs/design/device-controls.md 4, plan P23): a double click in any firmware opens it. Yay sits in
// the middle (r 56, "Yay" under it in Yay itself, "回到 Yay" in an extension); around it up to 5 places on a circle of
// r 142, clockwise from 12 o'clock every 72 degrees, each a dark disc (r 38) with its 64 x 64 icon and its name. The
// app in use has a sage ring and a sage name; one in the package cache a small hollow sage ring on the disc's lower
// right; one on neither (to download) is dimmed to 42 % with a small sage download mark. Since the app ecosystem's
// stage 1 (docs/design/app-ecosystem.md 4.5) the fifth place is 「更多」 (three dots): yk_apps_pick_ring() gives up
// to 4 apps and 「更多」 last; the callback then reports YK_APPS_MORE. Since 0.1.6 (docs/delivery/2026-10-25-settings-u.md
// 5) 「设置」 (a gear) comes after it, and a ring with it has 6 places every 60 degrees on a circle of r 128 (the name
// under the bottom one stays in the safe circle): apps from 12 o'clock, 「更多」 lower left, 「设置」 upper left. A ring
// without 「设置」 (an extension built before) keeps the 5 places at r 142.
// The background is opaque paper: nothing behind shows through (and LVGL skips drawing what is under it).
//
// Opening: the middle grows from 0.6, the apps fly out from the centre one after another (28 ms apart), about
// 300 ms in all. Closing: back into the centre, 180 ms. Untouched for 8 s it closes by itself.
//
// Objects exist only while it is open (one drawing object and Yay); icons are made in PSRAM when it opens and freed
// when it closes. Where an item gives no icon, the shared app catalog (yk_catalog.h) is asked; without one the
// name's first character stands in. Opening counts for the hint arrow's rules (yk_hint.h).
//
// What a tap does is the caller's (open the app, go home, install): this only reports it. Call under board_lock()
// (or from LVGL callbacks).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "yk_catalog.h"

#ifdef __cplusplus
extern "C" {
#endif

#define YK_APPS_MAX 6
#define YK_APPS_HOME "yay"    // the middle's slug (yaykit_app's YK_APP_HOME_SLUG)
#define YK_APPS_MORE "+more"  // 「更多」, the fifth place: all of one's apps (never an app's slug: those start with a letter)
#define YK_APPS_SETTINGS "+settings"  // 「设置」, the last place: Yay's settings
#define YK_APPS_RING_APPS 4   // apps on a ring with 「更多」

typedef struct {
    const char *slug;            // "breathe"; YK_APPS_MORE for 「更多」
    const char *name;            // "呼吸之间"; NULL: the catalog's name, else the slug (「更多」: "更多" / "More")
    bool installed;              // in a slot; false and not `cached`: dimmed, with the download mark
    const lv_image_dsc_t *icon;  // 64 x 64 in any format LVGL draws; NULL: the catalog's icon, else the first character
    bool cached;                 // not in a slot but in the package cache: not dimmed, a hollow sage ring on the disc
} yk_apps_item_t;

/** An installed app, from yaykit_app's slot table: its slug and when it was last used (Unix ms; 0 unknown). */
typedef struct {
    const char *slug;
    int64_t used_ms;
} yk_apps_slot_t;

/** The ring's apps in the design's order: the installed ones first, the most recently used first; then the
 *  catalog's apps that are not installed, in its order; at most `max` (<= YK_APPS_MAX). Yay, empty slugs and
 *  repeats are skipped. Slugs and names point into `installed` and `cat` (NULL: no catalog). Returns the count.
 *  Pure (no flash, no LVGL). */
int yk_apps_pick(const yk_apps_slot_t *installed, int n, const yk_catalog_t *cat, yk_apps_item_t *out, int max);
/** The ring with 「更多」 (app-ecosystem.md 4.5): up to YK_APPS_RING_APPS apps, those on the device first (in a slot,
 *  the most recently used first; then `cached`: the package cache's slugs, most recently used first), then the
 *  catalog's that are on neither, in its order; then 「更多」 (YK_APPS_MORE), always, and 「设置」 (YK_APPS_SETTINGS) last
 *  when there is room for both (`max` 2 or more). `cached` may be NULL. At most `max` items, those two included. Yay,
 *  empty slugs and repeats are skipped. Pure. */
int yk_apps_pick_ring(const yk_apps_slot_t *installed, int n, const char *const cached[], int nc,
                      const yk_catalog_t *cat, yk_apps_item_t *out, int max);

/** What was tapped, called once the ring has closed: the item's slug, YK_APPS_HOME for the middle, NULL for the
 *  paper around them or 8 s without a touch. Not called after yk_apps_close(). The ring is gone by then: the
 *  callback may open it again, draw a page, or restart. */
typedef void (*yk_apps_cb_t)(const char *slug, void *ctx);

/** Opens the ring over `parent` (the active screen: the ring is its top child; with the screen deleted the ring
 *  goes too, without a callback). `items` in the order shown, clockwise from 12 o'clock (yk_apps_pick() makes the
 *  order the design asks for); at most YK_APPS_MAX are used; fewer stand symmetrically (1: top; 2: upper left and
 *  right; 3: top and the lower pair; 4: all but the top). With 「更多」 last (yk_apps_pick_ring()), it takes the fifth
 *  place (upper left) and the apps the first ones from the top. With 「更多」 and 「设置」 last, the 6 places at r 128:
 *  the apps from the top, 「更多」 at 240 degrees, 「设置」 at 300. `current`: the slug in use (YK_APPS_HOME in Yay).
 *  The items are copied. Already open: closes that one at once (no callback) and opens anew. */
bool yk_apps_open(lv_obj_t *parent, const yk_apps_item_t *items, int n, const char *current, yk_apps_cb_t cb,
                  void *ctx);
/** Closes it without a callback: folding back in 180 ms (`animated`) or at once. Nothing when it is closed. */
void yk_apps_close(bool animated);
/** Open, or still folding back. */
bool yk_apps_is_open(void);

/** What the screen shows from a tap on the ring until the restart into the other firmware (design 4): 「正在打开」 and
 *  the app's name on black, or 「正在回到」 「Yay」 for YK_APPS_HOME (the prototype's), over everything (LVGL's top
 *  layer; touches stop there). `name` NULL: the catalog's, else the slug. Drawn by the next frame: let one pass before
 *  restarting. Under board_lock(). */
void yk_apps_leaving(const char *slug, const char *name);
/** Takes it away again: the restart did not happen (yk_app_open or yk_return_home failed). Under board_lock(). */
void yk_apps_leaving_clear(void);

/** 「回到 Yay 说？」 (device-controls.md 1.3): a hold in an app that has no conversation of its own and did not use
 *  it. A card on paper over the app (the prototype's): 「点屏幕回去 · 短按留下」. A tap anywhere calls back
 *  YK_APPS_HOME; 8 s untouched, NULL; either way it is gone by then. yk_apps_ask_close() (the key: stay) does not call
 *  back. Under board_lock(). */
bool yk_apps_ask(lv_obj_t *parent, yk_apps_cb_t cb, void *ctx);
void yk_apps_ask_close(void);
bool yk_apps_ask_is_open(void);

/** Development: as if `slug` were tapped on the open ring (YK_APPS_HOME the middle, NULL the paper around them; a slug
 *  it does not show counts as the paper), or anywhere on 「回到 Yay 说？」, so the device can be driven without a
 *  finger (`yay ring`). False when neither is open. Under board_lock(). */
bool yk_apps_tap(const char *slug);

/** Development: the ring's layout and the timings of the last open (`st apps` in shared-test). */
typedef struct {
    int setup_ms;      // inside yk_apps_open(): the catalog, the icons, the objects
    int icons_ms;      // ... of which reading and converting the icons
    int icons_cached;  // icons that came from the catalog
    int open_ms;       // from yk_apps_open() to the last app in place
    int frames;        // frames of the opening motion (about 300 ms)
} yk_apps_stats_t;
void yk_apps_stats(yk_apps_stats_t *out);

#ifdef __cplusplus
}
#endif

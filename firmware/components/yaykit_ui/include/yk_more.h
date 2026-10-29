// 「我的应用」 (docs/design/app-ecosystem.md 4.5, the design's round board 3): all of one's apps on a honeycomb,
// opaque over the whole screen. The one in the middle is big (100 px), the first ring 72, the second 54, those farther
// 40 and darker. Autofocus (docs/delivery/2026-10-26-more-autofocus.md, Apple Watch's way): sizes, light and a halo go
// by each app's distance from the middle on the screen, every frame of a drag, and the nearest one is the focus as it is
// dragged; let go, it settles with that one in the middle. A tap on the middle one, or on any app on the device (in a
// slot, in the package cache), reports it (opening or installing it is the caller's); one to download or coming soon
// is brought to the middle first (260 ms). Above, 「我的应用 · N」 (18); below, the middle one's name (22) and where it is (16,
// app-ecosystem.md 2: 在圆屏上 / 下载过 · 约 20 秒 (the copying) / 要下载 · 约 40 秒, from the last download's speed,
// or its size).
//
// Marks as on the app ring: in a slot none; in the package cache a hollow sage ring on the lower right; to download
// dimmed to 42 % with the sage download mark; in use a sage ring around it. On the app's disc its icon (Yay 0.1.5,
// #531: the store's 64 x 64 A8 in its colour, at 84, 61, 45 or 34 px by the disc's size), or without one the name's
// first character. Coming soon (即将推出): all of it at 42 %, no mark; the middle one tapped only shakes a little. A
// preview (the store has no stable release of it) says 预览版 before where it is.
//
// One drawing object over the screen, only while it is open; what each frame repaints is the honeycomb's band (or
// just the line below it). Under board_lock() (or from LVGL callbacks).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "yk_apps.h"
#include "yk_catalog.h"

#ifdef __cplusplus
extern "C" {
#endif

#define YK_MORE_MAX 64  // apps it shows (the owned list's 64, app-ecosystem.md 1.3 of the delivery)

typedef enum {
    YK_MORE_SLOT,   // in a slot: 在圆屏上
    YK_MORE_CACHE,  // in the package cache: 下载过 · 约 20 秒 (eta_s: the copying), or 下载过
    YK_MORE_CLOUD,  // neither: 要下载 · 约 40 秒 (eta_s), or 要下载 · 3.1 MB (size)
    YK_MORE_SOON,   // not out yet: 即将推出 (tapped in the middle it shakes; no callback)
} yk_more_where_t;

typedef struct {
    const char *slug;
    const char *name;  // NULL: the slug
    yk_more_where_t where;
    bool current;      // the app in use (正在用)
    bool preview;      // only a preview is out (the store's channel): 预览版 · …
    const char *glyph; // the character on its disc; NULL: the name's first
    uint32_t size;     // to download: its image's bytes (0: not known) ...
    int eta_s;         // ... and how long installing it would take, by the last download's speed (0: not measured);
                       // in the package cache: how long the copying takes (0: its size not known)
    const uint8_t *icon;  // its icon, 64 x 64 A8 (yk_catalog.h's YK_ICON_A8), drawn in icon_color; NULL: the first
    uint32_t icon_color;  // character. Read while the page is open: the caller keeps it until yk_more_close()
} yk_more_item_t;

/** The page's apps in the order of the delivery (2026-10-23-more-page.md 1.2): those on the device first (in a slot,
 *  the most recently used first; then `cached`, the package cache's slugs, most recently used first), then the
 *  catalog's that are on neither, in its order, then `extra`'s that are none of those (the apps planned or coming
 *  soon, as given). Yay, 「更多」, empty slugs and repeats are skipped; at most `max` (<= YK_MORE_MAX). Names from the
 *  catalog (NULL: not in it, the caller's); `preview` from the catalog's channel; `current` false. Slugs and names
 *  point into the arguments. Pure. */
int yk_more_pick(const yk_apps_slot_t *installed, int n, const char *const cached[], int nc, const yk_catalog_t *cat,
                 const yk_more_item_t *extra, int ne, yk_more_item_t *out, int max);

/** An estimate in words: 约 10 秒 up to 10, 约 20 … 50 秒 (up to the next ten) to 55, 约 1 分钟 to 89, 约 2 … 9 分钟
 *  (the nearest minute) to 9 min 29 s, then 10 分钟以上; English ~10 s, ~1 min, 10+ min. Returns out. Pure (Yay's
 *  install pages say it as the page does). */
const char *yk_more_eta_text(int eta_s, bool english, char *out, size_t size);

/** An app was tapped to be opened (the middle one, or one on the device anywhere): its index in the items given to
 *  yk_more_open(). The page stays open: the caller closes it (to open or install the app). */
typedef void (*yk_more_cb_t)(int index, void *ctx);

/** Opens it over `parent` (the active screen; its top child), the first item in the middle: `items` in the order
 *  wanted, most relevant first (the first in the middle, the next around it). Copied; at most YK_MORE_MAX. Already
 *  open: replaced. */
bool yk_more_open(lv_obj_t *parent, const yk_more_item_t *items, int n, yk_more_cb_t cb, void *ctx);
/** Takes it away (no callback). Nothing when closed. */
void yk_more_close(void);
bool yk_more_is_open(void);

/** Development (`yay apps tap|drag`): as a finger would, a tap on item `index` (as tapped(): reported, or moved to the
 *  middle), or a drag by (dx, dy) px over 0.3 s, then let go. False when it is not open,
 *  the item is not shown, or a finger is on it. Under board_lock(). */
bool yk_more_tap(int index);
bool yk_more_drag(int dx, int dy);
/** Development (the autofocus prototype's pictures): the honeycomb held dragged by (dx, dy) from where the hold began,
 *  as a finger that has not let go (the nearest one the focus); yk_more_let_go() lets it go (the nearest comes to the
 *  middle). False when it is not open (let go: not held). Under board_lock(). */
bool yk_more_hold(int dx, int dy);
bool yk_more_let_go(void);
/** Development: the halo's opacity (0–255; -1 back to YK_MORE_HALO_OPA's 30 %), for the comparison pictures. */
void yk_more_dev_halo(int opa);
/** The index in the middle (-1: closed). */
int yk_more_focus(void);

/** Development: how long opening took and how the frames went since it opened. */
typedef struct {
    int setup_ms;        // inside yk_more_open()
    int first_frame_ms;  // from yk_more_open() to its first frame on the panel
    int frames;          // frames drawn while it moved
    int frame_max_ms;    // the longest of them (render and send)
    int frame_avg_ms;
    int drag_max_ms;     // ... of those while a finger dragged it
    int move_max_ms;     // ... of those while an app moved to the middle
    int bands;           // a frame of those on average: the bands drawn (the display's height each at most) ...
    int kpx;             // ... the thousands of pixels in them ...
    int draw_ms;         // ... and the time drawing the apps and the words in them took (the rest: paper, sending)
    int shown_max;       // most apps drawn in one frame
} yk_more_stats_t;
void yk_more_stats(yk_more_stats_t *out);

#ifdef __cplusplus
}
#endif

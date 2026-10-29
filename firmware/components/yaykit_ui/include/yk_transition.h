// Into and out of an app (round-screen spec 8). Yay grows the app's icon from its hand to fill the screen, restarts
// into the app, and the app's first frame is exactly that full-bleed frame (the "splash"), which then opens into the
// app's home. Back (double click): the app shrinks into the splash, restarts into Yay, and Yay shrinks the icon back
// into its hand. The two sides draw the splash from the same description here, so the frames match.
//
// Each app's UI document names its splash: Breathe = a cream orb r 120 and a sage ring r 214 on black; Party Play =
// a coral disc with a die and confetti. Apps without one get a plain one (paper, sage ring) until they do.
//
// The black between the two firmware is at most a second: start the display dark (board_display_start_opts with
// start_dark), call yk_ui_first_frame() as the first thing drawn, and only then do the slow work. Under board_lock().
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *slug;
    uint32_t bg;              // the round screen behind everything
    uint32_t disc;            // a disc in the middle, radius disc_r (disc_r 0: none)
    int16_t disc_r;
    uint32_t ring;            // a ring at ring_r, ring_w wide (ring_w 0: none)
    int16_t ring_r, ring_w;
    /** The app's mark over the disc, scaled with it (Party Play's die and confetti); NULL: none. Drawn into the
     *  layer around (cx, cy) at `scale` (1 = the full-bleed frame). */
    void (*draw)(lv_layer_t *layer, int32_t cx, int32_t cy, float scale);
} yk_splash_t;

/** The splash of an app by slug; never NULL (a plain one for an app that has none yet). */
const yk_splash_t *yk_splash_for(const char *slug);

/** The splash as an object filling `parent` (466 x 466 at scale 1). */
lv_obj_t *yk_splash_create(lv_obj_t *parent, const yk_splash_t *splash);
/** Scales it about the screen's centre: 1 = full bleed; Yay's hand holds it at about 0.25. Only the splash's own
 *  area is repainted. */
void yk_splash_set_scale(lv_obj_t *splash, float scale);

/** The app's first frame: a new screen with the splash, loaded and sent to the panel now (lv_refr_now), then the
 *  panel turned up to the shared brightness (board_pref_brightness(); `brightness` >= 0 overrides it, e.g. a night
 *  scene's). Returns the screen. */
lv_obj_t *yk_ui_first_frame(const char *slug, int brightness);

/** From the first frame to `home` over ms (450 for Party Play, 700 for Breathe's own): the splash shrinks and fades
 *  over the home screen, which is loaded at once. */
void yk_ui_enter(lv_obj_t *home, int ms);

/** Back to Yay: the screen shrinks into the splash over ms (250..300). Returns at once; the caller waits ms (within
 *  the 400 ms the leave hook has), then restarts into Yay. */
void yk_ui_leave(const char *slug, int ms);

#ifdef __cplusplus
}
#endif

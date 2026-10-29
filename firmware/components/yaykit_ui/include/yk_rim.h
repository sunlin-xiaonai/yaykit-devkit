// The rim (round-screen spec 5): the round screen's common language, radius 226, 6 px, from the top clockwise. One
// state at a time; every app uses it with the same meaning. Drawn as 8 arcs of 45 degrees, each repainted only when
// it changed: a whole-ring restyle would make LVGL repaint the ring's bounding box, nearly the whole screen (the main
// firmware measured 71 ms a frame that way; a band of boxes is what it does now, yay-device.md 6.5).
//
//   LISTEN      sage, wider and brighter with the voice (yk_rim_level; restyled at most 4 times a second)
//   CONNECTING  a short ink3 arc turning slowly; never says "offline"
//   THINKING    a short sage arc sweeping back and forth (waiting for the cloud)
//   COUNTDOWN   coral, what is left (yk_rim_value: permille)
//   PROGRESS    sage, what is done (downloads, installs)
//   DIAL        ticks lit up to a step (volume, minutes): yk_rim_value = the step; the app plays the tick sound
//   DONE        sage closes to a full ring (from half, or from yk_rim_value: a progress that was there), then fades
//               out (about 0.8 s), then OFF by itself
//   HOLD        coral filling while a key is held (Breathe's hold-to-end): yk_rim_value permille
//   ALERT       one amber flash (battery low), then back to what was there
//
// A hero dial (Homework Buddy's timer) may be 12..16 wide (opts.width); listening and thinking still turn it sage,
// and it gets its own colour back after. Split screen (Translator lying flat): only the speaker's half lights
// (opts.from_deg / span_deg). Call under board_lock() (or from LVGL callbacks); it animates itself with an lv_timer.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    YK_RIM_OFF = 0,
    YK_RIM_LISTEN,
    YK_RIM_CONNECTING,
    YK_RIM_THINKING,
    YK_RIM_COUNTDOWN,
    YK_RIM_PROGRESS,
    YK_RIM_DIAL,
    YK_RIM_DONE,
    YK_RIM_HOLD,
    YK_RIM_ALERT,
    YK_RIM_MODE_COUNT,
} yk_rim_mode_t;

typedef struct {
    int width;         // px; 0 = 6 (YK_RIM_W). A hero dial: 12..16
    float from_deg;    // the part of the ring in use, clockwise from the top: from here ...
    float span_deg;    // ... this far (0 = the whole ring). The bottom half: from 90, span 180
    uint32_t color;    // 0 = the mode's own colour (above)
    int ticks;         // DIAL: steps on the ring (0 = 10)
    int opa;           // 0 = full; 1..255 to dim it (mixed with the paper, so it stays opaque)
} yk_rim_opts_t;

typedef struct yk_rim yk_rim_t;

/** A screen-sized, non-clickable object at the back of `parent` (a screen). */
yk_rim_t *yk_rim_create(lv_obj_t *parent);
void yk_rim_delete(yk_rim_t *rim);
lv_obj_t *yk_rim_obj(yk_rim_t *rim);

/** Changes the state (opts NULL: defaults). The same mode again keeps its animation going. */
void yk_rim_set(yk_rim_t *rim, yk_rim_mode_t mode, const yk_rim_opts_t *opts);
yk_rim_mode_t yk_rim_mode(const yk_rim_t *rim);
/** COUNTDOWN, PROGRESS, HOLD: permille 0..1000; DIAL: the step (0..ticks); DONE: where its closing starts (permille;
 *  0: half a ring). */
void yk_rim_value(yk_rim_t *rim, int value);
/** LISTEN: the microphone level 0..100 (yk_audio_level()). */
void yk_rim_level(yk_rim_t *rim, int level);
/** The paper behind it (YK_PAPER, or YK_NIGHT for a night screen): dimmed colours are mixed with it. */
void yk_rim_background(yk_rim_t *rim, uint32_t color);

/** Development: how many of the 8 boxes were repainted since the last call (the frame-cost check). */
int yk_rim_repaints(yk_rim_t *rim);

#ifdef __cplusplus
}
#endif

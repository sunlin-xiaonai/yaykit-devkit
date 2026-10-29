// Yay in an app (round-screen spec 7, extension platform P11): the character from yaykit/firmware/assets/yay (I8
// images with the face's boxes, yay_art.h), alive on its own: it blinks every 4..6 s, looks at whoever speaks or
// whose turn it is, opens its mouth with the voice (closed / slightly / open), changes pose with a short fade, and
// can be turned to any angle (Party Play peeks in from a player's edge; Translator faces the far side).
//
// Sizes: 264 hero, 176 companion, 132 corner. A pose that is not exported at a size is drawn from the nearest size
// that is, scaled (yk_yay_has() says whether it is native). Missing now: 132 for anything but greet, 264 for all but
// ready / resting / greet, thinking_glass at any size (the export script adds them; the names stay).
//
// Call under board_lock() (or from LVGL callbacks); blinking and breathing run on lv_timer / lv_anim.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    YK_YAY_READY = 0,
    YK_YAY_ACK,
    YK_YAY_LISTENING,
    YK_YAY_THINKING,
    YK_YAY_WORKING,
    YK_YAY_APPROVAL,
    YK_YAY_SPEAKING,
    YK_YAY_DONE,
    YK_YAY_REMINDER,
    YK_YAY_HELP,
    YK_YAY_RESTING,
    YK_YAY_GREET,
    YK_YAY_WORKING_LAPTOP,
    YK_YAY_THINKING_GLASS,  // Homework Buddy's "looking it up": until exported, drawn as THINKING
    YK_YAY_POSE_COUNT,
} yk_yay_pose_t;

typedef enum {
    YK_YAY_MOUTH_POSE = -1,  // the pose's own mouth
    YK_YAY_MOUTH_CLOSED = 0,
    YK_YAY_MOUTH_SLIGHT,
    YK_YAY_MOUTH_OPEN,
} yk_yay_mouth_t;

typedef struct yk_yay yk_yay_t;

/** `paper` for a Yay drawn over anything (Party Play peeking in over the wheel and the dice): the art keeps its
 *  alpha (RGB565 + an alpha plane, 3 bytes a pixel) instead of being blended with one colour. Costs more to draw
 *  than an opaque pose: use it where Yay overlaps other things, not on a plain background. */
#define YK_YAY_TRANSPARENT 0xFF000000u

/** Yay at `size` (132, 176 or 264) in `parent`, centred on (cx, cy) in the parent's coordinates, pose READY,
 *  blinking. The background under it is `paper` (YK_PAPER, YK_NIGHT...): the art is blended with it once (opaque
 *  RGB565, cheaper to draw), so put it on that colour; YK_YAY_TRANSPARENT keeps it transparent around Yay. */
yk_yay_t *yk_yay_create(lv_obj_t *parent, int size, int cx, int cy, uint32_t paper);
void yk_yay_delete(yk_yay_t *yay);
lv_obj_t *yk_yay_obj(yk_yay_t *yay);

/** Changes the pose, crossfading over fade_ms (0: at once). */
void yk_yay_pose(yk_yay_t *yay, yk_yay_pose_t pose, int fade_ms);
yk_yay_pose_t yk_yay_get_pose(const yk_yay_t *yay);
/** Changes the size (keeps the centre). */
void yk_yay_size(yk_yay_t *yay, int size);
void yk_yay_move(yk_yay_t *yay, int cx, int cy);

/** Blinks by itself every 4..6 s (on by default; off while a pose has its eyes shut). */
void yk_yay_auto_blink(yk_yay_t *yay, bool on);
void yk_yay_blink(yk_yay_t *yay);
/** Eyes towards a point on the screen (absolute coordinates); yk_yay_look_ahead() puts them back. */
void yk_yay_look_at(yk_yay_t *yay, int x, int y);
void yk_yay_look_ahead(yk_yay_t *yay);
/** The mouth: a fixed shape, or from what the speaker is playing now (yk_speak_level(), 0..100; call ~10 times a
 *  second while it speaks; a little hysteresis so it does not flutter). */
void yk_yay_mouth(yk_yay_t *yay, yk_yay_mouth_t mouth);
void yk_yay_mouth_level(yk_yay_t *yay, uint8_t level);
/** Breathing: 3 px up and down every 5 s (the idle screen's), off by default. */
void yk_yay_breathe(yk_yay_t *yay, bool on);

/** Turns Yay about its centre to `deg` (clockwise; 180 = upside down for the far side), over ms (0: at once).
 *  Rotated images cost more to draw: keep it still while turned, or small. */
void yk_yay_rotate(yk_yay_t *yay, float deg, int ms);

/** Whether the art has this pose at this size (otherwise it is scaled from another). */
bool yk_yay_has(yk_yay_pose_t pose, int size);
/** The pose's name as the art and the protocol spell it ("working-laptop"); NULL when out of range. And back:
 *  YK_YAY_POSE_COUNT for a name it does not know. */
const char *yk_yay_pose_name(yk_yay_pose_t pose);
yk_yay_pose_t yk_yay_pose_from_name(const char *name);

#ifdef __cplusplus
}
#endif

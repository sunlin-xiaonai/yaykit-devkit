// Pure C parts of yaykit_ui v2 (host tested with test/run.sh, LVGL stubbed): what the rim draws in each state and
// which of its 8 boxes changed, the subtitle layout by the circle, the notices' table, Yay's pose fallbacks, mouth
// and blinking.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "yk_notice.h"
#include "yk_rim.h"
#include "yk_yay.h"

// ---------------------------------------------------------------- the rim

#define YK_RIM_SEGS 8
#define YK_RIM_MAX_ARCS 64

typedef struct {
    float a0, a1;     // degrees clockwise from the top, 0 <= a0 < a1 <= 360
    uint32_t color;   // already mixed with the background when dimmed
    uint8_t width;
    bool round;       // rounded ends
} yk_rim_arc_t;

typedef struct {
    yk_rim_arc_t arc[YK_RIM_MAX_ARCS];
    int n;
} yk_rim_pic_t;

typedef struct {
    yk_rim_mode_t mode;
    yk_rim_opts_t opts;  // as given (zeros: defaults)
    int value;           // COUNTDOWN / PROGRESS / HOLD permille, DIAL step
    int level;           // LISTEN 0..100
    int64_t t_ms;        // since the mode was set (animations)
    uint32_t bg;         // the paper
} yk_rim_in_t;

#define YK_RIM_CONNECTING_PERIOD_MS 3000  // one turn
#define YK_RIM_THINKING_PERIOD_MS 1600    // there and back
#define YK_RIM_SHORT_ARC 40               // degrees
#define YK_RIM_DONE_CLOSE_MS 300
#define YK_RIM_DONE_FADE_MS 500
#define YK_RIM_ALERT_MS 600

/** What the rim draws for this state (arcs split at 360). */
void yk_rim_picture(const yk_rim_in_t *in, yk_rim_pic_t *out);
/** Whether an animated state is over (DONE faded out, ALERT flashed): the rim then goes OFF / back. */
bool yk_rim_finished(const yk_rim_in_t *in);
/** A fingerprint of what each 45-degree box shows: boxes whose fingerprint changed are repainted. */
void yk_rim_signatures(const yk_rim_pic_t *pic, uint32_t sig[YK_RIM_SEGS]);
/** The screen box of box `seg` for a band from r_out inwards `width` px (2 px more each way for antialiasing). */
void yk_rim_seg_box(int seg, int r_out, int width, int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2);
/** LISTEN: the step 0..4 for a level, and the width and opacity for a step (the main firmware's: 4..8 px). */
int yk_rim_listen_step(int level);
/** a mixed over b by opa 0..255 (opaque result). */
uint32_t yk_rim_mix(uint32_t a, uint32_t b, int opa);

// ---------------------------------------------------------------- subtitles

typedef int (*yk_glyph_width_fn)(uint32_t codepoint, void *ctx);

typedef struct {
    int start, len;  // bytes of the text
    int width;       // px of the glyphs
    int avail;       // px the circle allows for this line
    int y;           // the line's top, relative to the block's centre line
} yk_caption_line_t;

/** Width the circle of `radius` allows for a line spanning dy_top..dy_bottom from its centre (the farther edge
 *  counts): 2 x sqrt(r^2 - d^2), 0 outside. */
int yk_circle_width(int dy_top, int dy_bottom, int radius);

/** Lays `text` out in at most max_lines lines of line_h px, the block centred `center_dy` px below the circle's
 *  centre (negative: above). Each line as wide as the circle allows there. Returns the lines used, or -1 when it
 *  does not fit. lang "zh"/"ja" breaks between characters (not before closing punctuation; "ja" not before small
 *  kana or 'ー'), "en"/"es" at spaces (a word longer than a line is broken anywhere). */
int yk_caption_layout(const char *text, yk_glyph_width_fn width, void *ctx, int line_h, int center_dy, int radius,
                      int max_lines, const char *lang, yk_caption_line_t *lines);

/** One UTF-8 code point at p; *len its bytes (1 for a broken sequence). */
uint32_t yk_utf8_next(const char *p, int *len);

// ---------------------------------------------------------------- Yay

/** The pose and size to draw for a request: the size itself when the art has it, else the next larger one, else
 *  the largest smaller (scaled); THINKING_GLASS falls back to THINKING when it has none. has() answers for the
 *  art. Returns false when nothing at all exists for the pose. */
bool yk_yay_pick(yk_yay_pose_t pose, int size, bool (*has)(yk_yay_pose_t, int), yk_yay_pose_t *use_pose, int *use_size);

/** The art's palette (256 entries of B, G, R, A, alpha not premultiplied) as RGB565 for one image: blended over
 *  `paper` (0xRRGGBB) with alpha 255 everywhere, or for YK_YAY_TRANSPARENT the colours themselves with their alpha. */
void yk_yay_palette(const uint8_t pal[1024], uint32_t paper, uint16_t rgb[256], uint8_t alpha[256]);

/** Mouth steps from the speaker's level with hysteresis: opens at 45 / 18, closes below 35 / 10. */
yk_yay_mouth_t yk_yay_mouth_step(yk_yay_mouth_t now, uint8_t level);

/** Next blink in 4000..6000 ms (xorshift on *seed). */
int yk_yay_blink_gap_ms(uint32_t *seed);

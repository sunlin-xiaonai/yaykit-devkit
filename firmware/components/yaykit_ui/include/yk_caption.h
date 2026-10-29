// Subtitles and translations (round-screen spec 2 and 4): one sentence at a time, laid out horizontally, each line
// as wide as the circle allows at its distance from the centre (2 x sqrt(200^2 - d^2): 400 at the middle, 285 at
// 140 px away), 24 px, down to 20 when three lines do not hold it. Line breaks follow the language: Chinese and
// Japanese break between characters but never put closing punctuation at a line's start (nor Japanese small kana
// or the long-vowel mark), English and Spanish break at spaces. Changing the text crossfades in 150 ms.
//
// It works inside a turned container (yaykit_ui.h yk_ui_turn_box: Translator's far half), and two captions on one
// screen can use different glyph forms (lang "zh" vs "ja": the same code points drawn the Chinese or the Japanese
// way). Call under board_lock().
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int center_x, center_y;  // the block's middle in the parent's coordinates (each 0: the circle's centre); the
                             // circle's centre is the screen's (233, 233), or set `circle_*` for a turned box
    int circle_x, circle_y;  // the circle the lines must stay in, in the parent's coordinates (0, 0: 233, 233)
    int radius;              // 0 = 200 (the content circle)
    int max_lines;           // 0 = 3
    int size, small_size;    // 0 = 24, 20
    uint32_t color;          // 0 = ink
    const char *lang;        // "zh" (default), "ja", "en", "es": breaking rules and glyph forms
    int line_gap;            // extra px between lines (0 = 8)
} yk_caption_opts_t;

typedef struct yk_caption yk_caption_t;

yk_caption_t *yk_caption_create(lv_obj_t *parent, const yk_caption_opts_t *opts);
void yk_caption_delete(yk_caption_t *cap);
lv_obj_t *yk_caption_obj(yk_caption_t *cap);
/** Shows a sentence (crossfade 150 ms); NULL or "" fades it out. Too long even at the small size: the end is cut
 *  with an ellipsis (the app should split sentences). Returns the lines used. */
int yk_caption_set(yk_caption_t *cap, const char *text);
/** The same text without the fade (a word added as it is heard). */
int yk_caption_update(yk_caption_t *cap, const char *text);
void yk_caption_color(yk_caption_t *cap, uint32_t color);

#ifdef __cplusplus
}
#endif

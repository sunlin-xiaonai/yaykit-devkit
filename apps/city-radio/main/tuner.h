// The tuner face: an FM dial along the bottom of the round screen (87.5 .. 108 MHz, low on the left) with a dot per
// station and a coral needle that slides to the station tuned, the frequency large in the middle, and a row of level
// bars that move with what the speaker plays. LVGL: call with the board lock held (or from LVGL callbacks).
#pragma once

#include <stdbool.h>

#include "lvgl.h"

void tuner_create(lv_obj_t *screen);
/** Shows the station at this index; `slide` moves the needle there (else it jumps). */
void tuner_show_station(int index, bool slide);
/** Level bars: `level` 0..100 from yk_speak_level(); `live` false lays them flat and grey (paused, connecting). */
void tuner_level(int level, bool live);

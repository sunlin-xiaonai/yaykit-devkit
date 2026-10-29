// Inside yaykit_ui: what its files share (not for apps).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/** yk_ui_set_english() was called with true. */
bool yk_ui_is_english(void);

// Drawing shared by the app ring (yk_apps.c) and 「我的应用」 (yk_more.c). `k` scales a mark made for the ring's disc
// (r 38: YK_APPS_SAT_R) to a disc of another size.
/** A filled circle. */
void yk_draw_disc(lv_layer_t *layer, int cx, int cy, float r, uint32_t color, lv_opa_t opa);
/** The sage ring of what is in use: 3 px (times k), just outside a disc of radius r (times k). */
void yk_draw_in_use(lv_layer_t *layer, int cx, int cy, float r, float k, lv_opa_t opa);
/** A line of text centred in `a`. */
void yk_draw_text(lv_layer_t *layer, const char *str, const lv_font_t *font, uint32_t color, lv_opa_t opa,
                  const lv_area_t *a);
/** To download: a sage dot with a paper arrow pointing down, on the disc's lower right. */
void yk_draw_download_mark(lv_layer_t *layer, int cx, int cy, float k, lv_opa_t opa);
/** In the package cache: a hollow sage ring (paper inside) on the disc's lower right. */
void yk_draw_cached_mark(lv_layer_t *layer, int cx, int cy, float k, lv_opa_t opa);

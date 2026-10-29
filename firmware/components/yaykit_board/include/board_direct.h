/*
 * Drawing straight to the panel, around LVGL (自拍屏's viewfinder, docs/design/selfie.md 3). Added beside board.h;
 * nothing in board.c changed, and a firmware that never calls these draws exactly as before.
 *
 * board_direct_begin() pauses LVGL's drawing (esp_lvgl_adapter's "dummy draw": LVGL still runs its timers and
 * touches still reach its objects, but nothing it renders is sent). board_direct_blit() then sends big-endian RGB565
 * rows (the panel's byte order) from anywhere, PSRAM included, bounced through LVGL's own band buffer: internal,
 * DMA-capable and idle while LVGL does not send, so no internal RAM is added. Half the band is copied while the other
 * half is on its way. Measured on the device (10-28, the selfie lab): 466 x 466 in 18-20 ms, where LVGL drawing the
 * same picture took 62-70 ms. board_direct_end() gives the panel back to LVGL, which redraws the whole screen.
 *
 * Everything here under board_lock(). The panel's even-start rule holds: x, y, w and h even.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LVGL stops sending to the panel. ESP_ERR_INVALID_STATE before board_display_start(). Again: nothing. */
esp_err_t board_direct_begin(void);
/* LVGL sends again, the whole screen redrawn. Nothing when not begun. */
esp_err_t board_direct_end(void);
bool board_direct_active(void);

/* w x h pixels, big-endian RGB565, `stride` pixels a row (0: w), to (x, y) on the panel. `src` NULL sends black.
 * Returns once the panel has it all. ESP_ERR_INVALID_ARG for an odd or off-screen rectangle; ESP_ERR_INVALID_STATE
 * when not begun. */
esp_err_t board_direct_blit(const void *src, int stride, int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

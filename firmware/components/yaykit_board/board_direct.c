/*
 * Drawing straight to the panel (board_direct.h). Grown from the selfie lab's sl_view.c (exp/selfie-lab, c527e06c),
 * where 466 x 466 went from 62-70 ms through LVGL to 18-20 ms this way.
 */
#include "board_direct.h"

#include <string.h>

#include "board.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "lvgl.h"

static const char *TAG = "board_direct";

static bool s_on;

bool board_direct_active(void)
{
    return s_on;
}

esp_err_t board_direct_begin(void)
{
    if (s_on) {
        return ESP_OK;
    }
    lv_display_t *disp = lv_display_get_default();
    if (!disp) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = esp_lv_adapter_set_dummy_draw(disp, true);
    s_on = err == ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "begin: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t board_direct_end(void)
{
    if (!s_on) {
        return ESP_OK;
    }
    lv_display_t *disp = lv_display_get_default();
    const esp_err_t err = disp ? esp_lv_adapter_set_dummy_draw(disp, false) : ESP_ERR_INVALID_STATE;
    s_on = false;
    if (disp) {
        lv_obj_invalidate(lv_display_get_screen_active(disp)); /* the adapter redraws too; this makes sure */
    }
    return err;
}

esp_err_t board_direct_blit(const void *src, int stride, int x, int y, int w, int h)
{
    if (!s_on) {
        return ESP_ERR_INVALID_STATE;
    }
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > BOARD_LCD_H_RES || y + h > BOARD_LCD_V_RES || (x | y | w | h) & 1) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stride <= 0) {
        stride = w;
    }
    lv_display_t *disp = lv_display_get_default();
    lv_draw_buf_t *band = disp ? lv_display_get_buf_active(disp) : NULL;
    if (!band || !band->data) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Two halves of the band, 16-byte aligned; rows even (the panel's rule). LVGL does not touch the band while we
     * hold its lock, and the next blit's address command waits for the transfer before it. */
    const size_t half = (band->data_size / 2) & ~(size_t)15;
    const int rows = (int)(half / ((size_t)w * 2)) & ~1;
    if (rows < 2) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!src) {
        memset(band->data, 0, half);
    }
    const uint8_t *from = src;
    esp_err_t err = ESP_OK;
    for (int row = 0, k = 0; row < h && err == ESP_OK; row += rows, k++) {
        const int n = h - row < rows ? h - row : rows;
        uint8_t *dst = band->data + (src ? (k & 1) * half : 0);
        if (src) {
            for (int i = 0; i < n; i++) {
                memcpy(dst + (size_t)i * w * 2, from + ((size_t)(row + i) * stride) * 2, (size_t)w * 2);
            }
        }
        err = esp_lv_adapter_dummy_draw_blit(disp, x, y + row, x + w, y + row + n, dst, row + n >= h);
    }
    return err;
}

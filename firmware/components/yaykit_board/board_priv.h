// Inside yaykit_board: what its files share with each other (not for apps).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** One read of the touch controller's report (registers 0x01..0x06). ESP_ERR_INVALID_STATE without touch. */
esp_err_t board_touch_read(uint8_t report[6]);
/** While the fast reader runs: its latest state, so LVGL need not read the controller again. False otherwise. */
bool board_touch_fast_point(bool *pressed, int32_t *x, int32_t *y);

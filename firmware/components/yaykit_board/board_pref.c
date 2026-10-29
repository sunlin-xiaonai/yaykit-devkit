// The device's shared preferences (P10): volume, brightness, screen-off time in NVS namespace yk_pref, read and
// written by every firmware. Values are checked on the way in and out (yk_board_logic.c).
#include <stdbool.h>

#include "board.h"
#include "esp_log.h"
#include "nvs.h"
#include "yk_board_logic.h"

static const char *TAG = "board_pref";

#define NS "yk_pref"

static const char *const KEYS[] = {"volume", "bright", "screen_off"};

/** The stored value or -1. */
static int load(yk_pref_t which) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return -1;
    int32_t v = -1;
    if (nvs_get_i32(h, KEYS[which], &v) != ESP_OK) v = -1;
    nvs_close(h);
    return (int)v;
}

static esp_err_t store(yk_pref_t which, int value) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s not saved: %s", KEYS[which], esp_err_to_name(err));
        return err;
    }
    int32_t old = -1;
    if (nvs_get_i32(h, KEYS[which], &old) != ESP_OK || old != value) {  // no flash write for the same value
        err = nvs_set_i32(h, KEYS[which], value);
        if (err == ESP_OK) err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

int board_pref_volume(void) {
    int v = load(YK_PREF_VOLUME);
    if (v < 0) {
        // Before P10 the main firmware kept its volume in its own namespace: take it over once.
        nvs_handle_t h;
        uint8_t old = 0xFF;
        if (nvs_open("yay", NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_u8(h, "volume", &old) != ESP_OK) old = 0xFF;
            nvs_close(h);
        }
        if (old <= 10) {
            store(YK_PREF_VOLUME, old);
            return old;
        }
    }
    return yk_board_pref_read(YK_PREF_VOLUME, v);
}

esp_err_t board_pref_set_volume(int level) { return store(YK_PREF_VOLUME, yk_board_pref_clamp(YK_PREF_VOLUME, level)); }

int board_pref_brightness(void) { return yk_board_pref_read(YK_PREF_BRIGHTNESS, load(YK_PREF_BRIGHTNESS)); }

esp_err_t board_pref_set_brightness(int percent) {
    return store(YK_PREF_BRIGHTNESS, yk_board_pref_clamp(YK_PREF_BRIGHTNESS, percent));
}

int board_pref_screen_off_s(void) { return yk_board_pref_read(YK_PREF_SCREEN_OFF, load(YK_PREF_SCREEN_OFF)); }

esp_err_t board_pref_set_screen_off_s(int seconds) {
    return store(YK_PREF_SCREEN_OFF, yk_board_pref_clamp(YK_PREF_SCREEN_OFF, seconds));
}

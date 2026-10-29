#include "yk_board_logic.h"

void yk_board_round_area(int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2) {
    *x1 = (*x1 >> 1) << 1;
    *y1 = (*y1 >> 1) << 1;
    *x2 = ((*x2 >> 1) << 1) + 1;
    *y2 = ((*y2 >> 1) << 1) + 1;
}

bool yk_board_touch_point(const uint8_t report[6], int32_t width, int32_t height, int32_t *x, int32_t *y) {
    if ((report[1] & 0x0F) == 0) return false;
    int32_t px = ((report[2] & 0x0F) << 8) | report[3];
    int32_t py = ((report[4] & 0x0F) << 8) | report[5];
    *x = px >= width ? width - 1 : px;
    *y = py >= height ? height - 1 : py;
    return true;
}

bool yk_board_audio_rate_ok(uint32_t hz) {
    return hz == 16000 || hz == 24000;
}

int yk_board_battery_mv(uint8_t reg14, uint8_t reg15) { return (((reg15 & 0x0F) << 8) | reg14) * 12 / 10; }

bool yk_board_battery_known(int mv) { return mv >= 2500 && mv <= 4600; }

int yk_board_battery_percent(int mv) {
    static const struct {
        int mv, pct;
    } curve[] = {{3300, 0}, {3500, 5}, {3600, 10}, {3700, 25}, {3750, 40}, {3800, 50},
                 {3850, 60}, {3900, 70}, {4000, 80}, {4100, 90}, {4200, 100}};
    const int n = (int)(sizeof(curve) / sizeof(curve[0]));
    if (mv <= curve[0].mv) return 0;
    if (mv >= curve[n - 1].mv) return 100;
    for (int i = 1; i < n; i++)
        if (mv <= curve[i].mv)
            return curve[i - 1].pct + (mv - curve[i - 1].mv) * (curve[i].pct - curve[i - 1].pct) / (curve[i].mv - curve[i - 1].mv);
    return 100;
}

int yk_board_vbat_mv(int pin_mv) { return pin_mv * 2; }

bool yk_board_vbus_present(int pin_mv) { return pin_mv >= 1600; }

int yk_board_battery_level(int mv, bool charging) {
    if (!yk_board_battery_known(mv)) return -1;
    if (!charging) return yk_board_battery_percent(mv);
    const int pct = yk_board_battery_percent(mv - YK_BOARD_CHARGE_RISE_MV);
    return pct > 99 ? 99 : pct;
}

bool yk_board_battery_full(int mv) { return mv >= 4150; }

int yk_board_pref_default(yk_pref_t which) {
    switch (which) {
    case YK_PREF_VOLUME: return 6;
    case YK_PREF_BRIGHTNESS: return 80;
    default: return 30;  // the screen off after 30 s (P22; 180 before)
    }
}

int yk_board_pref_clamp(yk_pref_t which, int v) {
    switch (which) {
    case YK_PREF_VOLUME: return v < 0 ? 0 : v > 10 ? 10 : v;
    case YK_PREF_BRIGHTNESS: return v < 5 ? 5 : v > 100 ? 100 : v;
    default: return v <= 0 ? 0 : v < 15 ? 15 : v > 3600 ? 3600 : v;
    }
}

int yk_board_pref_read(yk_pref_t which, int stored) {
    if (stored < 0) return yk_board_pref_default(which);
    return yk_board_pref_clamp(which, stored) == stored ? stored : yk_board_pref_default(which);
}

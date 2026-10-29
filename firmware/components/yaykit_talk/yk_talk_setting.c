#include "yk_talk_setting.h"

static int clamp10(int v) { return v < 0 ? 0 : v > 10 ? 10 : v; }

int yk_talk_volume_after(int now, int level, int step, int mute, int *unmute, int fallback) {
    if (mute == 1) {
        if (now > 0) *unmute = now;
        return 0;
    }
    if (mute == 0) return now == 0 ? (*unmute > 0 ? *unmute : fallback) : now;  // not muted: as it is
    return step ? clamp10(now + step) : clamp10(level);
}

int yk_talk_brightness_level(int percent) { return percent <= 5 ? 0 : percent >= 100 ? 10 : (percent + 5) / 10; }

int yk_talk_brightness_percent(int level) {
    const int pct = clamp10(level) * 10;
    return pct < 5 ? 5 : pct;
}

int yk_talk_brightness_after(int percent, int level, int step) {
    if (level < 0 && !step) return -1;
    return step ? clamp10(yk_talk_brightness_level(percent) + step) : clamp10(level);
}

int yk_talk_brightness_lit(int percent, int battery_pct, bool usb) {
    const bool low = !usb && battery_pct >= 0 && battery_pct <= YK_TALK_LOW_BATTERY;
    return low && percent > YK_TALK_LOW_BRIGHTNESS ? YK_TALK_LOW_BRIGHTNESS : percent;
}

int yk_talk_screen_off_s(int seconds) {
    return seconds <= 0 ? 0 : seconds < 15 ? 15 : seconds > 3600 ? 3600 : seconds;
}

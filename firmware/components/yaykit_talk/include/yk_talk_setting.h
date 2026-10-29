// The device's settings as the voice actions take them (internal-commands.md 6.3, as the main firmware does since
// #264): volume.set and brightness.set by level, step or (volume) mute, worked out on the device where the value is
// known; screen_off.set. Plain C, host tested; yaykit_talk.c applies the result (yaykit_speak, board_pref).
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The volume after volume.set (yk_srv_msg_t: level 0..10 or -1, step -3..3 or 0, mute 1/0 or -1), from `now`.
 *  *unmute keeps the volume before a mute (RAM only: after a restart 「取消静音」 goes to `fallback`). */
int yk_talk_volume_after(int now, int level, int step, int mute, int *unmute, int fallback);
/** Brightness 0..10 as the actions take it, from the shared percent (5..100): level 0 is lit at 5 %. */
int yk_talk_brightness_level(int percent);
/** The percent for level 0..10: never under 5 (0 would look like a dead device). */
int yk_talk_brightness_percent(int level);
/** The level after brightness.set (level 0..10 or -1, step -3..3 or 0) from `percent`; -1: neither (an older
 *  server's "auto": there is no light sensor, answered UNSUPPORTED). */
int yk_talk_brightness_after(int percent, int level, int step);
#define YK_TALK_LOW_BATTERY 15     // percent, on battery: the panel lit at YK_TALK_LOW_BRIGHTNESS at most (as Yay)
#define YK_TALK_LOW_BRIGHTNESS 60  // percent

/** What the panel is lit at for the shared `percent`: at most YK_TALK_LOW_BRIGHTNESS on battery at or under
 *  YK_TALK_LOW_BATTERY (battery_pct < 0: not known, no cap). Yay caps from the first 15 % of a discharge until it is
 *  plugged in (yay_power); an extension only knows the reading now. */
int yk_talk_brightness_lit(int percent, int battery_pct, bool usb);
/** The screen-off time as the protocol takes it: 0 (never) or 15..3600 s. */
int yk_talk_screen_off_s(int seconds);

#ifdef __cplusplus
}
#endif

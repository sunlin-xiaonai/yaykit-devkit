#include "yk_talk_hello.h"

#include <string.h>

#include "yk_proto.h"
#include "yk_talk_setting.h"

// What yaykit_talk carries out by itself (internal-commands.md 5.4); "prefs" for yk_talk_prefs_changed(); "app.home":
// back to Yay when the phone asks (internal commands, step 2).
static const char *const CAPS[] = {"volume.step", "volume.mute", "brightness.step", "screen_off.set", "prefs", "app.home"};

char *yk_talk_hello(const yk_talk_hello_in_t *in) {
    const bool practice = in->mode && !strcmp(in->mode, "practice");
    const bool translate = in->mode && !strcmp(in->mode, "translate");
    if ((practice && (!in->language || !in->level)) || (translate && !in->pair)) return NULL;
    yk_proto_hello_t h = {
        .fw = in->fw,
        .board = "tikpal-r1",
        .battery_pct = in->battery_pct >= 0 ? in->battery_pct : 100,
        .charging = in->charging,
        .rssi = in->rssi > 0 ? -127 : in->rssi < -127 ? -127 : in->rssi,
        .locale = in->locale && in->locale[0] ? in->locale : "zh",
        .tz = "Asia/Shanghai",
        .out_rate = 16000,
        .mode = in->mode,
        .practice_language = practice ? in->language : NULL,
        .practice_level = practice ? in->level : NULL,
        .pair = translate ? in->pair : NULL,
        .has_prefs = true,
        .volume = in->volume < 0 ? 0 : in->volume > 10 ? 10 : in->volume,
        .brightness = yk_talk_brightness_level(in->brightness_pct),
        .screen_off_s = yk_talk_screen_off_s(in->screen_off_s),
        .caps = CAPS,
        .cap_count = sizeof(CAPS) / sizeof(CAPS[0]),
    };
    return yk_proto_hello(&h);
}

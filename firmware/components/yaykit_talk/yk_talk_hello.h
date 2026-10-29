// The hello an extension's session opens with (yay-device.md 4.3, extension-platform.md P1): plain C over yk_proto,
// host tested against the platform's own examples (test/run.sh).
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *fw;      // "language-buddy/0.1.0"
    const char *locale;  // the owner's language ("zh" when not known)
    int battery_pct;     // < 0: not read (sent as 100, as Yay does)
    bool charging;
    int rssi;            // > 0: not connected (sent as -127)
    int volume, brightness_pct, screen_off_s;  // the shared settings (board_pref)
    const char *mode;              // "practice", "translate"; NULL: chat (Yay's own)
    const char *language, *level;  // practice
    const char *pair;              // translate: "zh-ja"
} yk_talk_hello_in_t;

/** The JSON (cJSON_free), or NULL when the mode's fields break the schema (no language for practice…). Slots are
 *  an empty list: an extension does not install. caps: the setting actions it carries out itself. */
char *yk_talk_hello(const yk_talk_hello_in_t *in);

#ifdef __cplusplus
}
#endif

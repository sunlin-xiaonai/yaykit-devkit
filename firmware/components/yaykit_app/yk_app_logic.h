// Pure C decisions and record formats of yaykit_app, host tested with test/run.sh (no ESP-IDF needed).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "yk_app_summary.h"  // the summary and the record outbox (public, also pure)
#include "yk_app_update.h"   // the main firmware's update through the recovery (public, also pure)

#define YK_APP_RECORD_MAX 1024     // launch and result records, bytes
#define YK_APP_LAUNCH_MAX_AGE_S 60

/** Where "home" is: 0 (slot 1, ota_0) when it holds a usable main firmware, -1 for the recovery (factory).
 *  slot0_slug / slot0_state come from yk_slot_read() ("empty", "valid", "new", "pending", "invalid"...). */
int yk_home_slot(const char *slot0_slug, const char *slot0_state);

/** Recovery at boot: forward to slot 1 unless someone asked for the recovery or there is no usable Yay. */
bool yk_recovery_forward(bool recovery_flag, const char *slot0_slug, const char *slot0_state);

/** A launch record is for this app and fresh (60 s). Times before 2026 mean "unknown" and are not judged. */
bool yk_launch_accept(const char *record_slug, int64_t record_at_s, const char *my_slug, int64_t now_s);

/** JSON string escaping (without the quotes). Returns the length it needs; out is cut to fit and terminated. */
size_t yk_json_escape(char *out, size_t size, const char *in);

/** {"v":1,"from":..,"slug":..,"intent":..,"params":<object>,"at":..,"nonce":..}. params_json must be a JSON
 *  object (NULL = {}). Returns the length, or -1 when it does not fit in size or YK_APP_RECORD_MAX. */
int yk_launch_format(char *out, size_t size, const char *from, const char *slug, const char *intent,
                     const char *params_json, int64_t at_s, const char *nonce);

/** 「更多」 on the app ring: yaykit_ui's YK_APPS_MORE (the same string; a slug never starts with "+"). */
#define YK_RING_MORE "+more"
/** 「设置」 on the app ring (0.1.6, settings-u.md 5): yaykit_ui's YK_APPS_SETTINGS. */
#define YK_RING_SETTINGS "+settings"

/** What a tap on an extension's app ring does (yk_app_ring.c). */
typedef enum {
    YK_RING_STAY,       // nothing (the paper, 8 s, the app in use): the ring just closed
    YK_RING_HOME,       // back to Yay (the middle)
    YK_RING_OPEN,       // open that app's slot
    YK_RING_HOME_WITH,  // back to Yay with launch parameters: {"then":"more"} for 「更多」 (Yay opens 「我的应用」,
                        // app-ecosystem.md 4.5), {"then":"settings"} for 「设置」 (Yay opens its settings),
                        // {"then":"install","slug":…} for an app not on the device (only Yay installs)
} yk_ring_go_t;
/** `slug` tapped (NULL or "": nothing), `me` the app in use, `in_slot` whether that app is in a slot: what to do, and
 *  for YK_RING_HOME_WITH its parameters in `params` (a JSON object). */
yk_ring_go_t yk_ring_tapped(const char *slug, const char *me, bool in_slot, char *params, size_t size);

/** {"v":1,"slug":..,"summary":..,"data":<object>,"at":..}; summary may be NULL (left out), data NULL = {}. */
int yk_result_format(char *out, size_t size, const char *slug, const char *summary, const char *data_json,
                     int64_t at_s);

/** Start-up crash loop guard (docs/design/yay-device.md 6.9). */
#define YK_CRASHLOOP_LIMIT 3            // failed starts in a row before leaving the firmware
#define YK_CRASHLOOP_STABLE_MS 10000    // this long after yk_app_ready() counts as a good start

typedef enum {
    YK_GUARD_RUN,       // start normally
    YK_GUARD_HOME,      // an app that keeps crashing: back to Yay
    YK_GUARD_RECOVERY,  // the main firmware keeps crashing: the recovery
} yk_guard_action_t;

/** What the last reset says about the last run. */
typedef enum {
    YK_RESET_NORMAL,  // power on, esp_restart, USB, deep sleep: it ended on purpose, the count starts again
    YK_RESET_CRASH,   // panic, watchdog: one more failed start
    YK_RESET_POWER,   // brownout: the battery ran low, not the firmware's doing; the count stays as it was (neither
                      // one more nor forgotten: crashes before it still count)
} yk_reset_t;

/** `stored`: failed starts so far. Sets *count to the new count. */
yk_guard_action_t yk_boot_guard(uint8_t stored, yk_reset_t reset, bool is_home, uint8_t *count);

/** Chinese name of a built-in firmware for messages ("聚会玩" for "party-play"); the slug itself otherwise. */
const char *yk_app_display_name(const char *slug);
/** yaykit_app.h: the table's name in Chinese or English (NULL: not in it); the store's, the table's or the slug. */
const char *yk_app_table_name(const char *slug, bool english);
const char *yk_app_label(const char *slug, const char *catalog_name, bool english);

/** "YAYKIT_PARTY_PLAY_READY" for "party-play". */
void yk_banner_name(char *out, size_t size, const char *slug);

/** The side key in an extension with the app ring (P23, API level 3; device-controls.md 1.2, 1.3, 4): which presses
 *  are the app's and which the system's. A double click opens the ring; while the ring or 「回到 Yay 说？」 is up, the
 *  next press closes it and all of that press is the system's (its TAP, its HOLD, or the second half of a double
 *  click that would open the ring again). A hold the app did not use, in an app with no conversation of its own,
 *  asks 「回到 Yay 说？」. */
#define YK_KEYS_REOPEN_MS 1000  // a double click this soon after a press closed the ring is that press's: no ring

typedef struct {
    bool ring, ask;     // up now
    bool eat;           // the press that closed one of them goes on: the rest of it is the system's
    bool held;          // ... and it became a hold (its release ends it)
    int64_t closed_ms;  // when a press last closed the ring (-1: never)
} yk_keys_t;

typedef enum {
    YK_KEYS_APP,         // the app's: give it the event
    YK_KEYS_EAT,         // the system's, and nothing to do
    YK_KEYS_RING_OPEN,   // open the ring (the system's from now on)
    YK_KEYS_RING_CLOSE,  // fold it away
    YK_KEYS_ASK_CLOSE,   // 「回到 Yay 说？」: a press says stay
} yk_keys_do_t;

void yk_keys_init(yk_keys_t *k);
/** `type`: a yk_key_event_type_t (YK_KEY_DOWN 1 … YK_KEY_DOUBLE 5); `own_double`: the app takes the double click. */
yk_keys_do_t yk_keys_route(yk_keys_t *k, int type, int64_t at_ms, bool own_double);
/** After the app had a HOLD: true to ask 「回到 Yay 说？」 (it did not use it and `talks` is false). */
bool yk_keys_ask(yk_keys_t *k, bool used, bool talks);
/** The ring or the card went by itself (a tap on it, 8 s untouched). */
void yk_keys_closed(yk_keys_t *k);
/** Nothing of the system's is up or under way: the key may go back to the app's subscribers. */
bool yk_keys_idle(const yk_keys_t *k);

// Side-key event and configuration types (plain C, shared by the driver and the host-tested state machine).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    YK_KEY_DOWN = 1,
    YK_KEY_HOLD,
    YK_KEY_UP,
    YK_KEY_TAP,
    YK_KEY_DOUBLE,
} yk_key_event_type_t;

typedef struct {
    yk_key_event_type_t type;
    int64_t at_ms;    // when it happened (esp_timer clock, ms); for DOWN the edge itself
    int32_t held_ms;  // UP, TAP, DOUBLE: how long the key was down
} yk_key_event_t;

typedef struct {
    int hold_ms;       // 0 = 300
    int double_ms;     // 0 = 450
    bool no_double;    // every short press is a TAP (the main firmware)
    bool delay_tap;    // hold a TAP back until no double click can follow (for apps where both act)
    bool drop_tap_before_hold;  // with delay_tap: a tap still waiting when the next press becomes a hold is never
                                // sent (a quick tap, then talking, is one hold: the main firmware, P21)
} yk_key_config_t;

#ifdef __cplusplus
}
#endif

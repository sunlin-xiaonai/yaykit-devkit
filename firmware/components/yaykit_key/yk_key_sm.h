// The side key's state machine, pure C (no ESP-IDF), host tested with test/run.sh. The driver feeds it debounced
// edges and calls yk_key_sm_tick() at yk_key_sm_deadline(); it never reads the clock itself.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "yk_key_types.h"

#define YK_KEY_SM_MAX_EVENTS 4

typedef struct {
    yk_key_config_t cfg;
    bool pressed;
    bool hold_sent;
    bool second;           // this press may complete a double click
    bool tap_pending;      // delay_tap: a tap waits for the double-click window to close
    int64_t press_ms;
    int64_t tap_release_ms;  // release of the last tap, -1 when no double click can follow
    int32_t tap_held_ms;     // how long the pending tap was held
} yk_key_sm_t;

/** Defaults filled in for zero fields: hold 300 ms, double 450 ms. */
void yk_key_sm_init(yk_key_sm_t *sm, const yk_key_config_t *cfg);

/** A debounced change of the key at `now_ms`. Writes up to YK_KEY_SM_MAX_EVENTS events; returns how many. */
int yk_key_sm_edge(yk_key_sm_t *sm, bool pressed, int64_t now_ms, yk_key_event_t *out);

/** Time passing: emits HOLD once the key has been down hold_ms, and a delayed TAP. Returns the event count. */
int yk_key_sm_tick(yk_key_sm_t *sm, int64_t now_ms, yk_key_event_t *out);

/** When yk_key_sm_tick() must next run, or -1 when nothing is due. */
int64_t yk_key_sm_deadline(const yk_key_sm_t *sm);

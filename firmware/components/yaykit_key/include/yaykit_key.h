// The side key (GPIO0) for every Yay firmware: one state machine, events in order, callbacks from one task.
//   DOWN    the key went down (at once, no debounce delay: the main firmware opens the microphone here)
//   HOLD    held for hold_ms (300 ms): in the main firmware, talking starts
//   UP      released; held_ms says for how long
//   TAP     released before hold_ms
//   DOUBLE  a second tap that started within double_ms (450 ms) of the first tap's release; sent instead of its TAP
// With delay_tap a TAP waits until no DOUBLE can follow (450 ms after its release); with drop_tap_before_hold as
// well, a tap followed at once by a hold sends only the hold (down up down hold up: no TAP), as the main firmware does.
// By default yaykit_app takes DOUBLE: the app ring (P23; before API level 3, back to Yay). An app that takes it over
// must offer another way home.
// GPIO0 is also the download strapping pin: wait for it to be released (yk_key_wait_release) before any restart.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "yk_key_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*yk_key_cb_t)(const yk_key_event_t *event, void *ctx);

/** Starts the key task and the GPIO0 interrupt. cfg may be NULL. Idempotent. */
esp_err_t yk_key_init(const yk_key_config_t *cfg);

/** Up to 4 subscribers; each event goes to all, in order, from the key task. Keep callbacks short. */
esp_err_t yk_key_subscribe(yk_key_cb_t cb, void *ctx);

/** Every event goes to `cb` alone while it is set, none to the subscribers; NULL gives them back. yaykit_app takes
 *  the key this way while the app ring or 「回到 Yay 说？」 is up, so that a press closing them does nothing in the
 *  app. Takes effect from the next event (the one being given out still reaches the subscribers after the caller). */
void yk_key_grab(yk_key_cb_t cb, void *ctx);

/** Is the key down right now (raw level)? */
bool yk_key_pressed(void);

/** Waits until the key is up, at most timeout_ms. True if it is up. Works before yk_key_init() too. */
bool yk_key_wait_release(int timeout_ms);

/** Development: a press of hold_ms as if the key were pressed now (used by `yay tap`, `yay double`, `yay say`). */
esp_err_t yk_key_simulate(int hold_ms);

/** Name for logs: "down", "hold", "up", "tap", "double". */
const char *yk_key_event_name(yk_key_event_type_t type);

#ifdef __cplusplus
}
#endif

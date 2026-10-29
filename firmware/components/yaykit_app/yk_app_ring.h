// yaykit_app's side key in an extension (P23): the app ring and 「回到 Yay 说？」 (yk_app_ring.c). Private.
#pragma once

#include <stdbool.h>

#include "yaykit_app.h"

/** From yk_app_start(). `active`: an extension in a slot (Yay has its own ring; the recovery has nowhere to go):
 *  the double click opens the ring and a hold may ask. Otherwise the key only goes on to the app's yk_app_on_key. */
void yk_app_ring_start(const char *slug, bool talks, bool own_double, bool active);
/** Every key event (yaykit_app's subscriber, in the key task). */
void yk_app_ring_key(const yk_key_event_t *ev);
void yk_app_ring_on_key(yk_app_key_fn fn, void *ctx);
/** Development (`yay ring <slug>|yay|paper`): a tap on the open ring or 「回到 Yay 说？」. -1: not an extension's. */
int yk_app_ring_dev_tap(const char *what);

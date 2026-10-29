#include "yk_key_sm.h"

#include <string.h>

void yk_key_sm_init(yk_key_sm_t *sm, const yk_key_config_t *cfg) {
    memset(sm, 0, sizeof(*sm));
    if (cfg) sm->cfg = *cfg;
    if (sm->cfg.hold_ms <= 0) sm->cfg.hold_ms = 300;
    if (sm->cfg.double_ms <= 0) sm->cfg.double_ms = 450;
    sm->tap_release_ms = -1;
}

static int emit(yk_key_event_t *out, int n, yk_key_event_type_t type, int64_t at, int32_t held) {
    if (n < YK_KEY_SM_MAX_EVENTS) out[n] = (yk_key_event_t){.type = type, .at_ms = at, .held_ms = held};
    return n + 1;
}

static int send_pending_tap(yk_key_sm_t *sm, yk_key_event_t *out, int n) {
    sm->tap_pending = false;
    return emit(out, n, YK_KEY_TAP, sm->tap_release_ms, sm->tap_held_ms);
}

/** HOLD once the key has been down hold_ms; a tap still waiting for its pair is final then (and sent first), or
 *  with drop_tap_before_hold dropped: the hold is all there is. */
static int hold_if_due(yk_key_sm_t *sm, int64_t now_ms, yk_key_event_t *out, int n) {
    if (!sm->pressed || sm->hold_sent || now_ms - sm->press_ms < sm->cfg.hold_ms) return n;
    if (sm->tap_pending && sm->cfg.drop_tap_before_hold) sm->tap_pending = false;
    else if (sm->tap_pending) n = send_pending_tap(sm, out, n);
    sm->hold_sent = true;
    sm->second = false;  // a hold never completes a double click
    return emit(out, n, YK_KEY_HOLD, sm->press_ms + sm->cfg.hold_ms, sm->cfg.hold_ms);
}

int yk_key_sm_tick(yk_key_sm_t *sm, int64_t now_ms, yk_key_event_t *out) {
    int n = hold_if_due(sm, now_ms, out, 0);
    // With the key up and the window closed, no double click can follow any more.
    if (!sm->pressed && sm->tap_release_ms >= 0 && now_ms - sm->tap_release_ms > sm->cfg.double_ms) {
        if (sm->tap_pending) n = send_pending_tap(sm, out, n);
        sm->tap_release_ms = -1;
    }
    return n;
}

int yk_key_sm_edge(yk_key_sm_t *sm, bool pressed, int64_t now_ms, yk_key_event_t *out) {
    if (pressed == sm->pressed) return 0;  // repeated or bounced edge
    int n = yk_key_sm_tick(sm, now_ms, out);  // anything due before this edge comes first
    if (pressed) {
        sm->pressed = true;
        sm->hold_sent = false;
        sm->press_ms = now_ms;
        sm->second = !sm->cfg.no_double && sm->tap_release_ms >= 0 && now_ms - sm->tap_release_ms <= sm->cfg.double_ms;
        return emit(out, n, YK_KEY_DOWN, now_ms, 0);
    }
    n = hold_if_due(sm, now_ms, out, n);  // released after hold_ms but before a tick ran
    sm->pressed = false;
    const int32_t held = (int32_t)(now_ms - sm->press_ms);
    n = emit(out, n, YK_KEY_UP, now_ms, held);
    if (sm->hold_sent) {
        sm->tap_release_ms = -1;
    } else if (sm->second) {
        sm->second = false;
        sm->tap_pending = false;  // the first tap of the pair is not sent on its own
        sm->tap_release_ms = -1;
        n = emit(out, n, YK_KEY_DOUBLE, now_ms, held);
    } else if (sm->cfg.delay_tap && !sm->cfg.no_double) {
        sm->tap_pending = true;
        sm->tap_release_ms = now_ms;
        sm->tap_held_ms = held;
    } else {
        sm->tap_release_ms = sm->cfg.no_double ? -1 : now_ms;
        n = emit(out, n, YK_KEY_TAP, now_ms, held);
    }
    return n < YK_KEY_SM_MAX_EVENTS ? n : YK_KEY_SM_MAX_EVENTS;
}

int64_t yk_key_sm_deadline(const yk_key_sm_t *sm) {
    if (sm->pressed) return sm->hold_sent ? -1 : sm->press_ms + sm->cfg.hold_ms;
    if (sm->tap_release_ms >= 0) return sm->tap_release_ms + sm->cfg.double_ms + 1;
    return -1;
}

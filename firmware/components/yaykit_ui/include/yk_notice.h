// The same notices in every app (round-screen spec 9): the page stays, only Yay's pose and one short line change.
// No pop-ups. Offline features (timers, games, breathing) show nothing about the network at all.
//
//   CONNECTING    Yay waits, the rim turns ink3, no text; holding to talk still records
//   OFFLINE       after ~45 s of not connecting: "回 Yay 设置网络 / Set up Wi-Fi in Yay"
//   NOT_ADDED     "先在 Yay 里添加 / Add me in Yay first"
//   NO_CREDITS    "今天的点数用完了 / Out of points today" (offline features go on)
//   NOT_HEARD     help: "再说一次？ / Say it again?"
//   BUSY          help: "等一下再试 / Try again soon" (the app retries once by itself)
//   BLOCKED       help: "换个话题吧 / Let's try something else"
//   BATTERY_LOW   at 15 % and again at 10 %, once each a discharge: one amber flash of the rim, not while charging.
//                 The spec's row has no text: an app that shows the line (round-screen spec 9 since P25) passes it as
//                 the override, 「电量 15%，记得充电」 (the main firmware draws the same on its own rim)
//
// Wording may change for the audience, not the meaning (Homework Buddy says 「今天问够啦」 to a child): pass it as
// the override. Call under board_lock().
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "yk_rim.h"
#include "yk_yay.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    YK_NOTICE_NONE = 0,
    YK_NOTICE_CONNECTING,
    YK_NOTICE_OFFLINE,
    YK_NOTICE_NOT_ADDED,
    YK_NOTICE_NO_CREDITS,
    YK_NOTICE_NOT_HEARD,
    YK_NOTICE_BUSY,
    YK_NOTICE_BLOCKED,
    YK_NOTICE_BATTERY_LOW,
    YK_NOTICE_COUNT,
} yk_notice_t;

typedef struct {
    yk_yay_pose_t pose;   // Yay's pose for it (READY = "waits")
    yk_rim_mode_t rim;    // CONNECTING turns the rim, BATTERY_LOW flashes it; OFF = leave the rim alone
    const char *zh, *en;  // the line; NULL = no text
    bool retry_once;      // BUSY: the app tries again once by itself
} yk_notice_spec_t;

/** The spec's row. NONE and out of range: an empty row. */
const yk_notice_spec_t *yk_notice_spec(yk_notice_t notice);

#define YK_NOTICE_OFFLINE_AFTER_MS 45000

/** Network notices from the connection's state: CONNECTING while it tries, OFFLINE after 45 s of trying, NONE when
 *  connected. Pure; feed it whenever the state may have changed. */
typedef struct {
    int64_t trying_since_ms;  // 0: connected
} yk_notice_net_t;
yk_notice_t yk_notice_net(yk_notice_net_t *net, bool connected, int64_t now_ms);

typedef struct yk_notice_line yk_notice_line_t;

/** The line, centred at `y` (its middle; 0 = 360, under a 176 Yay) in `parent`, ink2 18 px, fading 150 ms. */
yk_notice_line_t *yk_notice_create(lv_obj_t *parent, int y);
/** Shows a notice: Yay's pose (when `yay` is given), the rim (when `rim` is given), the line (the override when
 *  not NULL, else the spec's text in the language). NONE clears the line and leaves Yay and the rim as they are. */
void yk_notice_show(yk_notice_line_t *line, yk_notice_t notice, yk_yay_t *yay, yk_rim_t *rim, const char *override,
                    bool english);
yk_notice_t yk_notice_current(const yk_notice_line_t *line);

#ifdef __cplusplus
}
#endif

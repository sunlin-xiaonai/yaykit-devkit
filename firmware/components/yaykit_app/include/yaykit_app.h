// The life of every Yay firmware (docs/design/yay-device.md 6.8–6.10, firmware guide 3–4):
//   yk_app_start()      right after NVS: the crash loop guard; records this slot's last use; the side key's
//                       double click opens the app ring (unless the app takes it, or it is the main firmware); the
//                       hint arrow's counts for this slug (yk_hint_init), and in an extension any press takes a
//                       hint away before the app hears of it (P24)
//   yk_app_on_key()     the app's side key through yaykit_app: a hold it does not use asks 「回到 Yay 说？」
//   yk_app_ready()      the screen is up: confirms the image so the bootloader keeps it; boot banner every 3 s
//                       until the first input
//   yk_app_launch()     launch parameters the main firmware left for this app (read once)
//   yk_app_set_result() a sentence and data for the main firmware, read when it starts again
//   yk_app_set_result_summary() the same with the summary Yay draws in its ring (yk_app_summary.h)
//   yk_app_on_leave()   the app's wrap-up (fade the sound, write the result), 400 ms at most, before leaving
//   yk_app_record()     a record for the platform, kept in the outbox until someone online delivers it
//   yk_app_open()       the main firmware opens another slot with parameters
//   yk_return_home()    back to Yay: slot 1 when it holds a usable main firmware, otherwise the recovery (factory)
//   yk_return_home_with() the same with a launch record for Yay ({"then":"install","slug":…} from the ring)
//   yk_app_recovery_read() the recovery's version from its description (yk_app_recovery.h); _hash() its image's sha256;
//                       _verify() whether it is whole, before Yay hands it an update
// Records live in NVS namespace yk_sys (launch, result, update, update_auto, update_bad, update_err, recovery,
// used_<slot>, boot_fail, crashed), shared by all firmware.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "yk_app_recovery.h"
#include "yk_app_summary.h"
#include "yk_key_types.h"
#include "yk_app_update.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Interface level of the shared components towards apps: only goes up, by one for each incompatible change.
 *  1: yaykit_key, yaykit_app (F1-a). 2: yaykit_cloud v2, the device API v1 (YK_CLOUD_API_LEVEL 2).
 *  3: the double click opens the app ring instead of going straight back to Yay (P23; built against level 2, an app
 *  still goes home on a double click); yk_app_on_key() and 「回到 Yay 说？」; yk_return_home_with(). */
#define YK_API_LEVEL 3

#define YK_APP_HOME_SLUG "yay"

typedef struct {
    const char *slug;     // "party-play"
    const char *version;  // "0.2.0"
    const char *banner;   // "YAYKIT_PARTY_PLAY_READY"
    bool is_home;         // the Yay main firmware: no app ring of yaykit_app's (Yay has its own)
    bool own_double;      // the app takes the double click itself and offers another way back to Yay
    bool quiet;           // the app prints its own banner (apps from before yaykit_app)
    bool talks;           // the app has a conversation of its own (API level 3): a hold is always its, never
                          // 「回到 Yay 说？」
} yk_app_config_t;

/** Call first, right after nvs_flash_init() and before the board, so a crash anywhere in start-up counts.
 *  In an app slot: after a panic, watchdog or brownout reset it counts a failed start, and on the third in a row
 *  (YK_CRASHLOOP_LIMIT) leaves instead of starting: an app goes back to Yay, the main firmware to the recovery,
 *  and `crashed` names it for whoever shows the message. Ten seconds after yk_app_ready() the count is cleared.
 *  Also: last-use record and double-click home (yaykit_key may start before or after). */
esp_err_t yk_app_start(const yk_app_config_t *cfg);

/** The app's side key through yaykit_app (API level 3), instead of yk_key_subscribe(): each event yaykit_key reports
 *  (from its task: keep it short), except the presses that open or close the app ring and 「回到 Yay 说？」. Return
 *  true when the app used it. A HOLD the app did not use, in an app without a conversation of its own (cfg.talks),
 *  asks 「回到 Yay 说？」 (device-controls.md 1.3): a tap on the screen goes back to Yay, a press stays. An app still
 *  on yk_key_subscribe() gets the ring all the same, never the question. After yk_app_start(). */
typedef bool (*yk_app_key_fn)(const yk_key_event_t *event, void *ctx);
esp_err_t yk_app_on_key(yk_app_key_fn fn, void *ctx);

/** Registers `app [status|exit|recovery|mem]` (mem: yk_mem.h) and, in development builds, `yay ...`. After esp_console_init(). */
esp_err_t yk_app_register_console(void);

/** The app started correctly (screen up): cancels the bootloader's pending rollback. Idempotent. */
esp_err_t yk_app_ready(void);

/** Something happened (input, a command): the banner stops repeating. Key events count by themselves. */
void yk_app_input(void);

typedef struct {
    bool present;
    char from[16];      // "yay"
    char intent[32];    // "practice"
    char nonce[17];
    int64_t at;         // Unix seconds when it was written
    char params[768];   // JSON object, "{}" when none
} yk_launch_t;

/** Reads and deletes this app's launch record. present = false when there is none, it is for another app, or
 *  it is older than 60 s. */
esp_err_t yk_app_launch(yk_launch_t *out);

/** Leaves a result for the main firmware. summary: one sentence for people (may be NULL); data_json: an
 *  object (may be NULL). At most 1 KB in all. */
esp_err_t yk_app_set_result(const char *summary, const char *data_json);

/** Leaves a result with the summary Yay draws when it is back (P3): data gets "summary" (yk_summary_format) next to
 *  the app's own fields in `data` (may be NULL). The sentence is what Yay says; it is not said when Yay comes back
 *  more than 30 minutes later (yk_result_fresh). */
esp_err_t yk_app_set_result_summary(const char *sentence, const yk_summary_t *summary, const cJSON *data);

/** The app's wrap-up before it leaves for Yay (a double click, `app exit`, yk_return_home, yk_app_recovery,
 *  yk_app_open): fade the sound out, write the result. It runs in a task of its own and gets YK_APP_LEAVE_MS; the
 *  restart does not wait longer, so it never cuts a sound mid-way for long (P3). NULL removes it. */
#define YK_APP_LEAVE_MS 400
typedef void (*yk_app_leave_fn)(void *ctx);
void yk_app_on_leave(yk_app_leave_fn fn, void *ctx);
/** The same with more time, up to YK_APP_LEAVE_MAX_MS, for a wrap-up that waits on the network: Language Buddy closes
 *  its connection after practice.end (#580, 10-25). Going back to Yay takes that much longer when it needs it all;
 *  less is never waited for. */
#define YK_APP_LEAVE_MAX_MS 1000
void yk_app_on_leave_ms(yk_app_leave_fn fn, void *ctx, uint32_t budget_ms);

/** The record outbox (P19): a record for the platform's records (POST /records), kept in the shared NVS until
 *  someone online delivers it (Yay over its session, or the app itself); the last YK_RECORD_SLOTS, a new one
 *  replacing the oldest. kind: "breathe.session"; data_json: an object of at most YK_RECORD_DATA_MAX bytes. */
esp_err_t yk_app_record(const char *kind, const char *data_json);
/** The records in the outbox, oldest first, from any app: fn gets each one's sequence number and its JSON
 *  (yk_record_format: the records API's fields plus "app"), until it returns false. Returns how many it got. */
typedef bool (*yk_app_record_fn)(uint32_t seq, const char *json, void *ctx);
int yk_app_records_each(yk_app_record_fn fn, void *ctx);
/** Removes the records up to and including `seq` (delivered). */
esp_err_t yk_app_records_drop(uint32_t seq);

/** Main firmware: takes the result an app left (JSON, deleted after reading). ESP_ERR_NOT_FOUND when none. */
esp_err_t yk_app_take_result(char *json, size_t size);

/** Main firmware: writes the launch record for `slug`, waits for the side key to be released, boots `slot`. */
esp_err_t yk_app_open(int slot, const char *slug, const char *intent, const char *params_json);

/** Back to Yay. Does not return when it works. */
esp_err_t yk_return_home(void);

/** Back to Yay with a launch record for it (yk_launch_t: `intent`, `params_json` an object): what Yay should do once it
 *  is up, e.g. {"then":"install","slug":"language-buddy"} from the app ring (P23). With no usable Yay (the recovery
 *  instead) the record is not read and goes stale. Does not return when it works. */
esp_err_t yk_return_home_with(const char *intent, const char *params_json);

/** Restarts into the recovery (factory) and keeps it there for this boot instead of forwarding to Yay. */
esp_err_t yk_app_recovery(void);

/** While held, leaving is refused: yk_app_open, yk_return_home(_with) and yk_app_recovery return ESP_ERR_INVALID_STATE,
 *  `yk cmd app.exit` / `app.recovery` and the console's `app exit` / `app recovery` answer BUSY. yaykit_install holds
 *  it while it writes a slot over the app it held (safe-replace ③); the firmware's own `reboot` should ask too. */
void yk_app_hold_leave(bool on);
bool yk_app_leave_held(void);

/** Recovery (factory) at boot: true when it should go straight on to slot 1. Clears the recovery flag. */
bool yk_app_should_forward(void);
/** Slot 1 holds a Yay the bootloader will start (not empty, not rolled back). */
bool yk_app_home_usable(void);

/** The firmware that last left because it kept crashing (its slug), read once: the main firmware and the
 *  recovery show "<name> 上次没有正常启动". ESP_ERR_NOT_FOUND when none. */
esp_err_t yk_app_take_crashed(char *slug, size_t size);

// ---------------------------------------------------------------- the main firmware's update (yk_app_update.h)

/** Yay: the new image (`version`, `sha256`) is in the package cache; the recovery writes it after the next restart
 *  (then yk_app_recovery()). `from`: the version running now. */
esp_err_t yk_app_update_request(const char *version, const char *sha256, const char *from);
/** The update record. ESP_ERR_NOT_FOUND when there is none (one not well formed is dropped). */
esp_err_t yk_app_update_get(yk_update_t *out);
/** The recovery's steps. */
esp_err_t yk_app_update_put(const yk_update_t *u);
void yk_app_update_clear(void);
/** The new Yay keeps the update: its record goes. yaykit_app calls it once the main firmware on trial has run
 *  YK_UPDATE_CONFIRM_MS after yk_app_ready(); Yay sooner, after hello.ok. Nothing when no update is on trial. */
void yk_app_update_confirm(void);
/** The recovery refused an update before slot 1 was touched (NO_SPACE: the previous version does not fit beside the
 *  new image): the record goes, and Yay hears the error code once (yk_app_update_take_error). */
void yk_app_update_fail(const char *code);
/** Why the last update was refused, read once. ESP_ERR_NOT_FOUND when none was. */
esp_err_t yk_app_update_take_error(char *code, size_t size);
/** The same without taking it (the recovery's `update status`). */
esp_err_t yk_app_update_peek_error(char *code, size_t size);
/** yk_app_update_request(), saying whether Yay asks for it on its own at night (automatic): NVS yk_sys "update_auto"
 *  then holds the record's sha256, and the recovery keeps the screen at its lowest (yk_app_update_is_auto). The
 *  record itself stays seven fields: a recovery before 0.1.6 drops one with more. yk_app_update_request() is this
 *  with automatic = false; clear, confirm and fail drop "update_auto" too. */
esp_err_t yk_app_update_request_ex(const char *version, const char *sha256, const char *from, bool automatic);
/** The record was asked for automatically ("update_auto" holds its sha256). */
bool yk_app_update_is_auto(const yk_update_t *u);
/** The recovery went back from this version: not offered again automatically (Yay's update check asks). */
esp_err_t yk_app_update_reject(const char *version);
bool yk_app_update_rejected(const char *version);
/** Forgets the version the recovery went back from (F, docs/delivery/2026-10-26-yay-update-nf.md 3.3): after a first
 *  failure Yay tries it again the next night; after the second it stays. */
void yk_app_update_unreject(void);

/** Chinese name of a built-in firmware ("聚会玩" for "party-play"); the slug itself otherwise. */
const char *yk_app_display_name(const char *slug);
/** A built-in firmware's name in Chinese or English (each app.json's), NULL when the table does not know it. */
const char *yk_app_table_name(const char *slug, bool english);
/** What to call an app on screen: `catalog_name` (the store's, in the device's language) when there is one, else the
 *  built-in name, else the slug ("" for none). The app ring, 「我的应用」 and Yay's install pages all name apps so. */
const char *yk_app_label(const char *slug, const char *catalog_name, bool english);

// ---------------------------------------------------------------- the recovery's version (yk_app_recovery.h)

/** The recovery (factory): its header and app description only (a few ms; no image read), looked up in the table of
 *  released recoveries (yk_recovery_identify). ESP_ERR_NOT_FOUND without a factory partition; an error when it holds
 *  no image. */
esp_err_t yk_app_recovery_read(yk_recovery_t *out);
/** The sha256 of the recovery's first `image_len` bytes (yk_recovery_t.size), 64 lowercase hex: the package .bin's.
 *  4 KB at a time, a 1 ms pause after each (1.6 MB: about 400 pauses), in a 4 KB internal buffer freed at the end.
 *  Blocks: call it from a low-priority task of its own, when nothing else is going on (Yay: once, when idle). */
esp_err_t yk_app_recovery_hash(uint32_t image_len, char sha256[65]);
/** The recovery is whole, before Yay hands it an update (docs/delivery/2026-10-26-yay-update-e.md 3): its `r->size`
 *  bytes read as yk_app_recovery_hash() does (4 KB, a 1 ms pause after each; never esp_image_verify's 1.6 MB in one
 *  go), the part before the SHA-256 the build appends matched against that SHA, and the whole image against
 *  `r->sha256` when it is known (the table, NVS). ESP_OK, ESP_ERR_INVALID_CRC (not whole: no SHA appended, or a
 *  mismatch), or a read error. The whole image's sha256 into `sha256` either way once read ("" when not read).
 *  Blocks as _hash() does. */
esp_err_t yk_app_recovery_verify(const yk_recovery_t *r, char sha256[65]);

/** When a slot was last opened, Unix ms; 0 when not known. */
int64_t yk_app_last_used(int slot);

/** `yk cmd` handling shared by all apps: app.exit, app.recovery, app.status. Returns ESP_ERR_NOT_SUPPORTED for
 *  other names so the app can go on with its own. */
esp_err_t yk_app_command(const char *name, const cJSON *args, cJSON *reply, const char **code);

/** Development builds (CONFIG_YAY_DEV): adds `yay <name> ...` (the main firmware adds state, action, inbox,
 *  server). Built-in: tap, double, hold <ms>, say <s> [text], records [drop <seq>], launch <slug> [json],
 *  crash [boots] (panics now and at the start of the next `boots` boots, to try the crash loop guard), record.
 *  Release builds keep only tap, double, hold, say and records (CONFIG_YAY_ACCEPT); what is registered here is not
 *  reachable there. */
typedef int (*yk_app_dev_fn)(int argc, char **argv);
esp_err_t yk_app_dev_register(const char *name, const char *help, yk_app_dev_fn fn);
/** Development builds: `yay say <s> <text>` passes the text here (the main firmware hands it to its session). */
typedef void (*yk_app_say_fn)(int seconds, const char *text);
void yk_app_dev_on_say(yk_app_say_fn fn);

#ifdef __cplusplus
}
#endif

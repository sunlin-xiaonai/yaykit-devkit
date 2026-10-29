// The main firmware's update through the recovery (docs/design/yay-release-update.md 3.3–3.5): Yay downloads the new
// image into the package cache and leaves a record (NVS yk_sys "update"), then restarts into the recovery. The
// recovery copies the Yay that ran into the cache as the previous version, writes the new one into slot 1 and starts
// it; when the new one keeps crashing at start, or slot 1 ends up holding no usable Yay, the previous one goes back.
// Pure C (host tested): the record's format and the recovery's next step.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    YK_UPDATE_NONE,
    YK_UPDATE_PENDING,    // the new image is in the package cache, asked for by Yay
    YK_UPDATE_BACKED_UP,  // the recovery copied the Yay that ran into the cache as the previous version
    YK_UPDATE_TRIAL,      // the new version is in slot 1 and starts; confirmed after a stable minute, or hello.ok
} yk_update_state_t;

#define YK_UPDATE_TRIES 2          // writes of the new image before going back (a power cut halfway is one)
#define YK_UPDATE_CONFIRM_MS 60000  // a new Yay that has run this long after yk_app_ready() keeps the update

typedef struct {
    yk_update_state_t state;
    char version[32];  // the new one
    char sha256[65];   // its image, 64 lowercase hex
    char from[32];     // the version it replaces ("" when not known)
    uint8_t tries;     // writes begun
} yk_update_t;

/** The record as stored: "1|yay|<version>|<sha256>|<state>|<tries>|<from>". Returns its length, or -1 when the record
 *  is not well formed or does not fit. */
int yk_update_format(char *out, size_t size, const yk_update_t *u);

/** False when it is not a record this firmware writes (it is then dropped). */
bool yk_update_parse(const char *in, yk_update_t *out);

/** What the recovery knows at start. */
typedef struct {
    bool has;              // an update record
    yk_update_t rec;
    bool home_usable;      // slot 1 holds a Yay the bootloader will start
    bool home_crashed;     // the recovery came up because Yay kept crashing at start
    bool new_cached;       // the record's image is in the package cache
    bool previous_cached;  // the package cache holds the previous version
} yk_update_in_t;

typedef enum {
    YK_UPDATE_GO,        // nothing to do: on as before (forward to Yay, or the recovery page)
    YK_UPDATE_BACKUP,    // copy slot 1 into the cache as the previous version (then BACKED_UP: write)
    YK_UPDATE_WRITE,     // write the new image into slot 1 (then TRIAL: start it)
    YK_UPDATE_RESTORE,   // slot 1 holds no usable Yay and there is no update going: the previous version back
    YK_UPDATE_ROLLBACK,  // the update did not take (keeps crashing, could not be written): the previous one back,
                         // the new version remembered as bad, the record dropped
    YK_UPDATE_DROP,      // the record can not go on and there is nothing to go back to: forget it
    YK_UPDATE_REJECT,    // the new image could not be written (refused before slot 1 was touched) and the Yay there
                         // still works: forget the record, remember the new version as bad
} yk_update_step_t;

/** The recovery's next step. Called again after each one until it says GO. */
yk_update_step_t yk_update_next(const yk_update_in_t *in);

/** How long after yk_app_ready() (its self-check passed: the screen is up) the main firmware confirms the update it
 *  runs: YK_UPDATE_CONFIRM_MS when it is on trial, -1 when there is nothing to confirm (`u` NULL: no record) or this is
 *  not the main firmware in slot 1. hello.ok confirms sooner. Without it a Yay that never got online would stay on
 *  trial, and a crash loop much later, for another reason, would send it back to the previous version. */
int32_t yk_update_confirm_after(const yk_update_t *u, bool home);

/** Whether the update still needs the package cache's new image and previous version (neither is dropped for room;
 *  an image that does not fit beside them is refused, NO_SPACE): from the request until it is confirmed or dropped. */
bool yk_update_holds_cache(const yk_update_t *u);

// The new recovery (docs/delivery/2026-10-25-recovery-p3.md): a low battery stops it before it touches a working
// slot 1, and an update Yay asked for on its own at night (NVS yk_sys "update_auto" holding the record's sha256, kept
// out of the record itself: a recovery before 0.1.6 drops a record with an eighth field) keeps the screen dark.
#define YK_UPDATE_LOW_BATTERY 30  // percent: under it, on battery, the recovery does not write slot 1

/** The recovery may write slot 1 now: on external power, or at least YK_UPDATE_LOW_BATTERY, or not known (-1: Yay
 *  looked before it handed the update over). */
bool yk_update_battery_ok(int percent, bool usb);
/** This step would touch a slot 1 that still works (BACKUP, or the first WRITE while slot 1 is usable): the ones a low
 *  battery stops. A WRITE after a power cut, RESTORE and ROLLBACK go on whatever the battery says. */
bool yk_update_step_guarded(yk_update_step_t step, const yk_update_in_t *in);
/** `auto_sha` (the "update_auto" value, NULL or "" when none) marks this record as asked for automatically. */
bool yk_update_auto_matches(const char *auto_sha, const yk_update_t *u);

const char *yk_update_state_name(yk_update_state_t state);
const char *yk_update_step_name(yk_update_step_t step);

#ifdef __cplusplus
}
#endif

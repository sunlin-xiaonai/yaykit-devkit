// Installs app images into slots (docs/design/yay-device.md 6.8, firmware guide 5): downloaded over HTTPS (resuming a
// dropped connection), copied from the package cache, or streamed over USB. Before a slot can boot its image is
// checked: size, SHA-256, the signature (yaykit_sig: the release keys; development builds also the developer key) and
// the project name. The slot's first sector is written last, so an interrupted or wrong install leaves the slot empty,
// never half written. The boot partition is not changed: the caller opens the app afterwards (yk_app_open).
//
// yk_install_start goes in three stages (docs/delivery/2026-10-24-safe-replace.md), so that the app a slot held is
// never lost: ① the new image into the package cache, whole and checked (or found there); ② the app the slot holds, when
// the cache has no copy of it, copied in beside it; ③ the slot written from the cache. A cancel, a failure, low battery
// or leaving in ① and ② loses only what was coming. ③ is not cancelled and leaving is refused (yk_app_hold_leave,
// BUSY); should it fail, the old app is put back at once, and a power cut or a crash leaves a record that the next start
// acts on (yk_install_resume). 位置 1 (Yay) is not protected this way: its update has its own (yk_app_update.h). The
// USB `install` command does not go through this either: interrupted, it leaves its slot empty.
//
// The package cache (`pkgcache`, 10 MB above 16 MB) keeps the images of the apps installed most recently: an image
// goes into it while it downloads, and installing it again copies it from there, with no network. When it is full,
// the least recently used images make room, except the two the main firmware's update needs (its new image and the
// previous version, from the request until the new Yay is confirmed). The index lives in NVS namespace yk_pkgcache.
//
// Yay: on app.install, yk_install_plan() says where the app goes and what it replaces (the confirmation page), then
// yk_install_start() with a listener that draws the progress ring; on YK_INSTALL_DONE, yk_app_open(). Hold the voice
// session closed while downloading (yk_session_hold): installing and talking exclude each other (6.3).
// One install at a time. Flash writes stall both cores for a moment (a 64 KB erase is about 0.25 s).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define YK_INSTALL_BOARD "tikpal-r1"
#define YK_INSTALL_LAYOUT "yaykit-v1"

/** What to install: app.install's fields, or a /device/v1/apps item. */
typedef struct {
    char slug[32];
    char name[48];        // for people ("呼吸之间"); may be empty
    char version[32];
    uint32_t size;
    char sha256[65];      // 64 lowercase hex
    char url[384];        // https:// (development builds: http:// too); empty: from the package cache only
    char signature[112];  // "yks1.<key id>.<r‖s>" (firmware/signing/README.md)
} yk_install_manifest_t;

typedef enum {
    YK_INSTALL_IDLE,
    YK_INSTALL_DOWNLOADING,  // from the network into the package cache (①)
    YK_INSTALL_COPYING,      // within flash: the replaced app into the cache (②), the cache into the slot (③)
    YK_INSTALL_VERIFYING,
    YK_INSTALL_DONE,
    YK_INSTALL_FAILED,
} yk_install_phase_t;

typedef enum {
    YK_INSTALL_STAGE_FETCH,    // ① the new image into the package cache (or found there); may be cancelled
    YK_INSTALL_STAGE_BACKUP,   // ② the app it replaces into the package cache; may be cancelled
    YK_INSTALL_STAGE_WRITE,    // ③ the slot, from the cache: not cancelled, leaving refused
    YK_INSTALL_STAGE_RESTORE,  // the replaced app back into its slot (③ failed, or at start after a power cut)
} yk_install_stage_t;

/** What became of the app the slot held. */
typedef enum {
    YK_INSTALL_OLD_NONE,      // the slot was empty (or it is 位置 1)
    YK_INSTALL_OLD_KEPT,      // untouched: the install stopped before ③
    YK_INSTALL_OLD_REPLACED,  // the new app took its place
    YK_INSTALL_OLD_RESTORED,  // the new app could not be written: the old one is back
    YK_INSTALL_OLD_LOST,      // it could not be put back
    YK_INSTALL_OLD_PENDING,   // not back yet: the record stays, the next start puts it back (yk_install_resume)
} yk_install_old_t;

typedef struct {
    yk_install_phase_t phase;
    yk_install_stage_t stage;
    int slot;              // 0..2 (位置 1–3)
    char slug[32];         // being written (while putting an app back: that app)
    char version[32];
    bool from_cache;
    uint32_t done, total;  // bytes through all three stages: the progress ring is done / total
    const char *error;     // when failed: a stable code (below)
    yk_install_old_t old;
    char old_slug[32];     // the app the slot held
    bool old_signed;       // a signed copy of it is in the package cache: it can come back any time
} yk_install_status_t;

/** Called from the install task on every phase change and at most every 200 ms while bytes flow. Keep it short (post
 *  to the UI). The last call has phase DONE or FAILED. */
typedef void (*yk_install_listener_t)(const yk_install_status_t *status, void *ctx);

/** Where an app would go, for the confirmation page. */
typedef struct {
    int slot;             // 1 or 2 for an app (位置 2/3), 0 for Yay; -1: nowhere (error says why)
    bool update;          // the same app is there now (another version)
    bool replaces;        // another app is there now and goes
    char replace[32];     // its slug (yk_app_display_name() for its name)
    char replace_version[32];  // the version there now (update or replaces)
    char replace_sha256[65];   // that image's SHA-256 when this component installed it (the build in the slot is still
                               // the one it noted), else "": not known without reading the whole slot
    bool replace_signed;  // a signed copy of that very image is in the package cache: it can come back any time
    bool cached;          // this exact image is in the package cache: no download
    const char *error;    // NO_FREE_SLOT, SLOT_IN_USE, BAD_REQUEST
} yk_install_plan_t;

/*
 * Error codes (stable; for logs and app.install's action.done — people see yk_install_error_text()):
 *   BAD_REQUEST, NOT_HTTPS, TOO_LARGE      the manifest
 *   NO_SPACE                               the package cache: too little room beside the update's images
 *   UPDATING                               the same, while the main firmware updates: the new app and the backup of the
 *                                          one it replaces do not fit beside its two images (said before downloading)
 *   BUSY                                   also: an app waits to be put back into its slot (a record left for the next
 *                                          start); nothing is installed until it is
 *   BAD_SIGNATURE                          not signed by a key this build trusts (the log says which check failed)
 *   NO_FREE_SLOT, NO_SUCH_SLOT, SLOT_IN_USE where it would go
 *   HOME_SLOT_RESERVED                     位置 1 (slot 0) takes the Yay main firmware only, and Yay no other slot
 *   BUSY                                   another install is running
 *   NOT_CACHED                             no URL and not in the package cache
 *   NO_WIFI, NOT_FOUND, DOWNLOAD_FAILED    the network
 *   SIZE_MISMATCH, HASH_MISMATCH           the bytes are not the ones signed
 *   BAD_IMAGE, WRONG_APP                   not a bootable image, or another app's
 *   FLASH_ERROR, NO_MEMORY, CANCELLED
 *   LOW_BATTERY                            not by this component: the caller refuses at 15 % on battery (Yay, P25),
 *                                          power lost halfway would leave the slot empty
 */

/** After nvs_flash_init(): reads the package cache's index. */
esp_err_t yk_install_init(void);

/** Checks a manifest's fields, its URL (https) and its signature against the keys this build trusts. NULL when it may
 *  be installed, else an error code. Takes about 3 KB of stack and ~100 ms (ECDSA). */
const char *yk_install_check(const yk_install_manifest_t *m);

/** Fills a manifest from JSON with app.install's / the catalog's fields (slug, name, version, size, sha256, url,
 *  signature; unknown ones ignored). NULL or BAD_REQUEST. Does not check the signature. */
const char *yk_install_from_json(const cJSON *obj, yk_install_manifest_t *m);

/** Where `m` would go. `replace`: the app the server named (app.install's replace), may be NULL. */
void yk_install_plan(const yk_install_manifest_t *m, const char *replace, yk_install_plan_t *out);

/** Checks `m` (yk_install_check) and starts installing it into `slot` in the background, in the three stages above.
 *  ESP_ERR_INVALID_STATE while another install runs; ESP_ERR_INVALID_ARG when the check fails (the listener hears
 *  FAILED with the code first). The listener may be NULL. */
esp_err_t yk_install_start(const yk_install_manifest_t *m, int slot, yk_install_listener_t listener, void *ctx);
/** The same; `replaced` (may be NULL): the store's release of the app the slot holds now. When its SHA-256 is that of
 *  the slot's image, the backup (②) carries its signature: that app can then come back any time, like any download. */
esp_err_t yk_install_start_replacing(const yk_install_manifest_t *m, int slot, const yk_install_manifest_t *replaced,
                                     yk_install_listener_t listener, void *ctx);

/** At start, after yk_install_init(), before any install: a replacement that a power cut or a crash interrupted in ③
 *  is finished. The slot holds an app: the record goes. It is empty: the old app goes back from the package cache in
 *  the background, heard by the listener as an install (stage RESTORE; DONE with old RESTORED, or FAILED with old
 *  LOST), and so it does when it can not (no backup, or tried YK_REPLACE_TRIES times: FAILED, old LOST, at once).
 *  ESP_OK when the listener will hear it; ESP_ERR_NOT_FOUND when there was nothing to do. */
esp_err_t yk_install_resume(yk_install_listener_t listener, void *ctx);
/** Whether yk_install_resume() would have something for the listener: a replacement cut short is on record and its
 *  slot does not hold an app. Read only (the record, the slot's image, the cache's index), nothing started. Yay starts
 *  its session held closed then (docs/delivery/2026-10-25-yay-0.1.4.md 7: its WebSocket beside the restore took
 *  internal RAM to 40 KB). */
bool yk_install_resume_pending(void);

/** Stops the running install at its next chunk (FAILED, CANCELLED) in ① and ②: the slot keeps its app. In ③ and
 *  while an app is put back it is not stopped: ESP_ERR_INVALID_STATE. */
esp_err_t yk_install_cancel(void);
/** In ③ or putting an app back: not cancelled, leaving refused. */
bool yk_install_writing(void);
bool yk_install_busy(void);
void yk_install_get_status(yk_install_status_t *out);
/** {"phase","stage","slot","slug","version","done","total","fromCache","error","old","oldSlug"} (idle: only phase). */
void yk_install_status_json(cJSON *obj);

/** A short sentence for people (≤ 10 characters in Chinese): "没有通过校验" for any check that failed. */
const char *yk_install_error_text(const char *code, bool english);

/** One image in the package cache. */
typedef struct {
    char slug[32];
    char name[48];
    char version[32];
    uint32_t size;
} yk_install_cached_t;

/** The cached images, most recently used first; not the backups without a signature. Returns how many (≤ max). */
int yk_install_cache_list(yk_install_cached_t *out, int max);
/** The newest signed cached image of `slug` as a manifest with no URL: to install it again with no network.
 *  ESP_ERR_NOT_FOUND when none. */
esp_err_t yk_install_cache_manifest(const char *slug, yk_install_manifest_t *out);
/** Forgets `slug`'s cached images (NULL: all of them), but not the ones an update going needs. */
esp_err_t yk_install_cache_forget(const char *slug);
/** {"capacity","used","items":[{"slug","name","version","size"}]}. */
void yk_install_cache_json(cJSON *obj);

// ---------------------------------------------------------------- the main firmware's update (yk_app_update.h)

/** The package cache's copy of the Yay that ran before an update: its own slug (a download of "yay" never replaces
 *  it, and it is never offered for an install), and no signature (yk_install_check refuses it). */
#define YK_INSTALL_PREVIOUS "yay-previous"
#define YK_INSTALL_PREVIOUS_SIGNATURE "previous"

/** The recovery, before it writes an update (docs/design/yay-release-update.md 3.3): copies the Yay in slot 1 into the
 *  package cache as the previous version, replacing an older copy. The cache's least recently used images make room,
 *  never the update's new image: NO_SPACE when the rest is not enough (then nothing was dropped, and the update is
 *  refused). Blocks for a few seconds, showing as COPYING in yk_install_get_status. NULL or an error code (NO_SPACE,
 *  BAD_IMAGE, TOO_LARGE, FLASH_ERROR, BUSY, NO_MEMORY). */
const char *yk_install_backup_home(void);
/** The recovery: the previous version back into slot 1. No signature: it is the copy of a Yay that ran there; its
 *  SHA-256 and the image are checked (a bad copy is dropped). Blocks. NULL, NOT_CACHED or an install error. */
const char *yk_install_restore_home(void);
/** Whether the package cache holds the previous version; its version into `version` (may be NULL). */
bool yk_install_previous(char *version, size_t size);
/** The cached image of `slug` with this SHA-256 as a manifest with no URL (for yk_install_start). ESP_ERR_NOT_FOUND. */
esp_err_t yk_install_cache_manifest_sha(const char *slug, const char *sha256, yk_install_manifest_t *out);

/**
 * Stage ① alone (docs/delivery/2026-10-26-yay-update-e.md 2.2): `m` checked (yk_install_check, before anything is
 * downloaded), downloaded into the package cache, its size and SHA-256 checked, no slot written. For Yay's own update,
 * which the recovery writes (it finds the image by its SHA-256: yk_install_cache_manifest_sha). `beside`: bytes that
 * must still fit next to it (the previous Yay the recovery will back up), asked for first (nothing comes that could not
 * be used). `make_room`: the least recently used images may go for both, as for an install (a tap); else only `m`'s
 * slug's other images and YK_INSTALL_PREVIOUS (the background: never an app's), NO_SPACE when that is not enough.
 * Already cached and whole: done at once. A cancel (yk_install_cancel) stops it with CANCELLED; nothing resumes later.
 * Status and listener as yk_install_start's: stage FETCH, slot -1 (none). ESP_ERR_INVALID_STATE while an install runs.
 */
esp_err_t yk_install_fetch(const yk_install_manifest_t *m, uint32_t beside, bool make_room,
                           yk_install_listener_t listener, void *ctx);

/** What yk_install_fetch_room may push out of the package cache for the image and `beside`. */
typedef enum {
    YK_FETCH_ROOM_OWN,        // only `m`'s slug's other images and YK_INSTALL_PREVIOUS (yk_install_fetch, make_room false)
    YK_FETCH_ROOM_ANY,        // the least recently used, as for an install (a tap; yk_install_fetch, make_room true)
    YK_FETCH_ROOM_APPS_LAST,  // any, but first the copies of apps in no slot, then the least recently used of the apps in
                              // the slots (Yay's night and background update, 0.1.10): the apps in the slots stay, only
                              // their cached copies go (installing one again then downloads it)
} yk_fetch_room_t;

/** yk_install_fetch with the room it may make. Each image dropped for it: `YK_INSTALL_CACHE drop slug=… version=… size=…
 *  slot=0|1` on the console (slot 1: the app is in a slot, only its cached copy went). */
esp_err_t yk_install_fetch_room(const yk_install_manifest_t *m, uint32_t beside, yk_fetch_room_t room,
                                yk_install_listener_t listener, void *ctx);
/** The cached image of `slug` with this SHA-256 still there and whole: read back and hashed, a 1 ms pause after each
 *  4 KB (no idle task starved, #555), and `beside` bytes still fitting next to it by yk_install_fetch's rules. Blocks
 *  (~3 s for 3.3 MB): call it from a low-priority task. NULL, NOT_CACHED, HASH_MISMATCH (the copy is dropped then),
 *  NO_SPACE, TOO_LARGE, FLASH_ERROR or NO_MEMORY. */
const char *yk_install_cache_check(const char *slug, const char *sha256, uint32_t beside, bool make_room);
/** yk_install_cache_check with the room `beside` may make (yk_fetch_room_t, as yk_install_fetch_room's). */
const char *yk_install_cache_check_room(const char *slug, const char *sha256, uint32_t beside, yk_fetch_room_t room);

/** After esp_console_init(). `install <slot 0-2> <size> <sha256> [slug]`: an image streamed raw over USB (the
 *  launcher's protocol, used by yay_dev.py and the desktop app; no signature, not cached: whoever holds the USB cable
 *  can flash the chip anyway). Slot 0 needs the slug "yay"; other slots refuse it (HOME_SLOT_RESERVED).
 *  `pkg [status|cache]`, and in development builds `pkg install '<json>' [slot] [open]`,
 *  `pkg cached <slug> [slot] [open]`, `pkg cache-slot <slot> <signature>`, `pkg forget <slug>|--all` (all only with `--all`), `pkg fail <n>` (the
 *  next n download attempts fail, as a network that is gone: DOWNLOAD_FAILED after the retries). */
esp_err_t yk_install_register_console(void);

#ifdef __cplusplus
}
#endif

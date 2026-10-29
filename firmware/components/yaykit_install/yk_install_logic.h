// The pure parts of yaykit_install, host tested (test/run.sh): manifest fields, project names, which slot an app goes
// to, the package cache's index, and the writer that fills a slot or a cache extent through a flash interface.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mbedtls/sha256.h"

#define YK_INST_SLOTS 3
#define YK_INST_HOME_SLOT 0           // 位置 1: the main firmware
#define YK_INST_MAX_IMAGE 0x380000    // a slot, 3.5 MB
#define YK_INST_SECTOR 4096
#define YK_INST_BLOCK 65536
#define YK_INST_CACHE_MAX 6           // entries in the package cache's index

// ---------------------------------------------------------------- manifest

/** NULL when the fields can be installed (and signed: firmware/signing/README.md), else BAD_REQUEST, NOT_HTTPS or
 *  TOO_LARGE. `allow_http`: development builds take http:// too, for a test server on the bench. */
const char *yk_inst_check_fields(const char *slug, const char *version, uint32_t size, const char *sha256,
                                 const char *url, const char *signature, bool allow_http);
/** a-z, 0-9 and '-', 1–31 characters, starting with a letter or digit. */
bool yk_inst_slug_ok(const char *slug);
/** What `pkg forget` (development builds) is asked: argv as the console gives it ("pkg", "forget", …). Nothing after
 *  it is not "everything" (10-25: a line cut short as Yay started emptied a test device's cache): only `--all` is;
 *  one slug is that app's images; anything else, the usage. */
typedef enum { YK_FORGET_USAGE, YK_FORGET_ALL, YK_FORGET_SLUG } yk_forget_t;
yk_forget_t yk_inst_forget_args(int argc, char **argv, const char **slug);
/** 64 lowercase hex characters into 32 bytes. */
bool yk_inst_hex32(const char *hex, uint8_t out[32]);
/** Image project names are yaykit_<slug with underscores>. */
bool yk_inst_project_matches(const char *project, const char *slug);

// ---------------------------------------------------------------- where an app goes

/** Slot 0 (位置 1) holds the main firmware ("yay"), and the main firmware lives nowhere else: NULL when `slug` may go
 *  into `slot`, else HOME_SLOT_RESERVED. An empty slug (a USB install that names none) passes here and is judged by
 *  the image's project name once it has arrived. */
const char *yk_inst_slot_rule(int slot, const char *slug);
/** The same rule for an image's project name (yaykit_<slug>), checked before its first sector is written. */
const char *yk_inst_slot_rule_project(int slot, const char *project);

typedef struct {
    bool present;    // holds an app
    bool running;    // the firmware doing the install runs from it
    bool broken;     // the bootloader gave up on it (invalid, aborted)
    char slug[32];
    int64_t used;    // when it was last opened (yk_app_last_used), 0 when not known
} yk_inst_slot_t;

typedef struct {
    int slot;             // -1: nowhere (error says why)
    bool update;          // the same app is there now
    bool replaces;        // another app is there now and goes
    char replace[32];     // that app
    const char *error;    // NO_FREE_SLOT, BAD_REQUEST
} yk_inst_choice_t;

/** Where `slug` goes. The main firmware ("yay") only into slot 0; apps into 1 and 2: the one with the same app, else an
 *  empty one, else the one `replace` names (what Yay told the user), else a broken one, else the least recently
 *  opened. Never the slot the installer runs from. */
void yk_inst_choose(const yk_inst_slot_t slots[YK_INST_SLOTS], const char *slug, const char *replace,
                    yk_inst_choice_t *out);

// ---------------------------------------------------------------- package cache index

typedef struct {
    char slug[32];
    char name[48];
    char version[32];
    char sha256[65];
    char signature[112];
    uint32_t size;
    uint32_t offset;   // in the pkgcache partition, a multiple of YK_INST_BLOCK
    uint32_t seq;      // larger = used more recently
} yk_cache_entry_t;

typedef struct {
    uint32_t seq;
    int count;
    yk_cache_entry_t e[YK_INST_CACHE_MAX];
} yk_cache_index_t;

/** The entry holding exactly this image, or -1. */
int yk_cache_find(const yk_cache_index_t *ix, const char *slug, const char *sha256);
/** The most recently used entry for `slug`, or -1. */
int yk_cache_newest(const yk_cache_index_t *ix, const char *slug);
/** Marks entry `i` as just used. */
void yk_cache_touch(yk_cache_index_t *ix, int i);
void yk_cache_remove(yk_cache_index_t *ix, int i);
/** Images that stay whatever needs room: while the main firmware updates (yk_app_update.h), its new image and the
 *  previous version; while an app takes the place of another (yaykit_install's three stages), its new image and the
 *  backup of the one it replaces. */
#define YK_CACHE_KEEP_MAX 4
typedef struct {
    const char *sha256[YK_CACHE_KEEP_MAX];  // these images, of any slug; NULL: none
    const char *slug;                       // every image of this slug; NULL: none
    const char *only[2];  // set: only images of these slugs may go to make room (Yay's update downloading in the
                          // background: its own older images, never an app's); none set: any the rest does not hold
    const char *last[3];  // these slugs' images go only once every other one that may go has gone, the least recently
                          // used first among them too (the apps in the slots, when Yay's night update makes room: the
                          // copies of apps in no slot go first); NULL: none
} yk_cache_keep_t;

/** Whether `keep` (may be NULL) holds entry `e`. */
bool yk_cache_kept(const yk_cache_entry_t *e, const yk_cache_keep_t *keep);
/** One more image `keep` holds (NULL `sha256`, or no room left: nothing). */
void yk_cache_keep_add(yk_cache_keep_t *keep, const char *sha256);

/** The signature of a backup that has none (a slot's app installed over USB): it only goes back into its own slot
 *  when the app that replaced it could not be written, and is never offered for an install. */
#define YK_CACHE_BACKUP_SIGNATURE "backup"
bool yk_cache_is_backup(const yk_cache_entry_t *e);

#define YK_CACHE_NEVER (-1)  // larger than the whole region
#define YK_CACHE_FULL (-2)   // fits only by dropping what `keep` holds

/** Room for `size` bytes in a region of `capacity`: the lowest offset (a multiple of YK_INST_BLOCK) where it fits once
 *  the entries of slug `replace` (may be NULL: this image takes their place; never the image keep->sha256 names) and
 *  then the least recently used entries `keep` does not hold (one more when the index is full) are dropped, those of
 *  keep->last's slugs after all the others. Returns
 *  the offset, YK_CACHE_NEVER or YK_CACHE_FULL; when it does not fit, nothing is dropped. `dropped` (may be NULL)
 *  counts the entries that went. */
int64_t yk_cache_place(yk_cache_index_t *ix, uint32_t size, uint32_t capacity, const yk_cache_keep_t *keep,
                       const char *replace, int *dropped);
/** Whether an image of `first` bytes and then one of `second` beside it would fit (0: that one is not needed), by
 *  yk_cache_place's rules, `keep` holding both once placed. The index is not changed. 0, YK_CACHE_FULL or
 *  YK_CACHE_NEVER: asked before a download, so that nothing comes that can not be used (the new app and the backup of
 *  the one it replaces). */
int yk_cache_fits(const yk_cache_index_t *ix, uint32_t first, uint32_t second, uint32_t capacity,
                  const yk_cache_keep_t *keep);
/** Adds an entry (after its image is written and checked) as the most recently used. */
void yk_cache_add(yk_cache_index_t *ix, const yk_cache_entry_t *entry);
/** Order for people: most recently used first. Writes up to `max` indexes into `order`, returns how many. */
int yk_cache_order(const yk_cache_index_t *ix, int *order, int max);

/** The index as stored (NVS blob). Returns its length, or 0 when `size` is too small. */
size_t yk_cache_encode(const yk_cache_index_t *ix, uint8_t *out, size_t size);
/** Reads a stored index, keeping only entries that are well formed, inside `capacity` and not overlapping an earlier
 *  one. False (and an empty index) when the blob is not an index at all. */
bool yk_cache_decode(const uint8_t *in, size_t len, uint32_t capacity, yk_cache_index_t *ix);
#define YK_CACHE_BLOB_MAX (16 + YK_INST_CACHE_MAX * sizeof(yk_cache_entry_t))

// ---------------------------------------------------------------- replacing an app (yaykit_install.h, three stages)

/** Kept in NVS while a slot is written over the app it held, from just before its first block is erased until the new
 *  app is checked there (or the old one is back): a power cut or a crash in between leaves this, and the next start
 *  puts the old app back from the package cache (yk_replace_next). */
typedef enum {
    YK_REPLACE_NONE,
    YK_REPLACE_WRITING,    // the new app is being written into the slot
    YK_REPLACE_RESTORING,  // the old one is being put back
} yk_replace_state_t;

#define YK_REPLACE_TRIES 3  // starts that try to put the old app back before it is given up

typedef struct {
    yk_replace_state_t state;
    int slot;
    char new_slug[32], new_sha256[65];
    char old_slug[32], old_sha256[65];  // the backup in the package cache
    uint8_t tries;                      // times the old app was being put back
} yk_replace_t;

/** "1|<slot>|<new slug>|<new sha256>|<old slug>|<old sha256>|<state>|<tries>". Its length, or -1 when the record is
 *  not well formed or does not fit. */
int yk_replace_format(char *out, size_t size, const yk_replace_t *r);
/** False when it is not a record this firmware writes (it is then dropped). */
bool yk_replace_parse(const char *in, yk_replace_t *out);

typedef struct {
    bool has;            // a record
    yk_replace_t rec;
    bool slot_valid;     // the slot holds an image the bootloader would start (whichever app it is)
    bool backup_cached;  // the package cache holds the old app's image (rec.old_slug, rec.old_sha256)
} yk_replace_in_t;

typedef enum {
    YK_REPLACE_GO,       // nothing to do
    YK_REPLACE_CLEAR,    // the slot holds an app (the new one written, the old one never touched, or another one
                         // installed since): the record goes, the slot is left alone
    YK_REPLACE_RESTORE,  // the slot is empty or half written: the old app back from the cache
    YK_REPLACE_LOST,     // no backup, or it was tried YK_REPLACE_TRIES times: the record goes, the app is gone
} yk_replace_step_t;

/** At start: what to do about a replacement that did not finish. */
yk_replace_step_t yk_replace_next(const yk_replace_in_t *in);

/** Putting the old app back failed now (not a power cut: a write or check that went wrong), `r->tries` counting this
 *  one. True: keep the record, the next start tries again; false: give up (the backup is bad, or it was tried
 *  YK_REPLACE_TRIES times). */
bool yk_replace_retry(const yk_replace_t *r, bool backup_ok);
#define YK_REPLACE_TEXT_MAX 256  // a record with the longest slugs is about 215 characters
const char *yk_replace_step_name(yk_replace_step_t step);

// ---------------------------------------------------------------- cancel and ③ (one lock)

/** What a cancel and the start of ③ decide on, under the install's lock: a cancel that came first stops it before the
 *  slot is touched; once ③ began, a cancel is refused. */
typedef struct {
    bool busy;     // an install runs
    bool cancel;   // asked to stop
    bool writing;  // ③ (or an app being put back): not stopped
} yk_inst_flags_t;

/** A cancel: false (refused) when nothing runs or the slot is being written. */
bool yk_inst_cancel(yk_inst_flags_t *f);
/** The start of ③: false when a cancel came first (the install ends CANCELLED, the slot untouched). */
bool yk_inst_begin_write(yk_inst_flags_t *f);

// ---------------------------------------------------------------- writer

/** Flash operations on one partition, offsets from its start; each returns 0 when it worked. */
typedef struct {
    int (*erase)(void *ctx, uint32_t offset, uint32_t len);
    int (*write)(void *ctx, uint32_t offset, const void *data, uint32_t len);
    int (*read)(void *ctx, uint32_t offset, void *data, uint32_t len);
    void *ctx;
    uint32_t capacity;
} yk_flash_t;

/**
 * Writes one image at `base` of a partition, hashing what it writes. Flash is erased just ahead of the data (64 KB
 * blocks where aligned, skipped when already blank). In head-last mode (a slot) the first sector is erased at once
 * and kept in RAM until yk_writer_commit_head(): until then the slot holds no app, so an interrupted install leaves
 * it empty, never half written.
 */
typedef struct {
    yk_flash_t flash;
    uint32_t base, size, written, erased;
    uint8_t *head;           // head-last: the first sector (caller's buffer, YK_INST_SECTOR bytes)
    uint8_t *scratch;        // for blank checks (caller's buffer, YK_INST_SECTOR bytes)
    mbedtls_sha256_context sha;
    bool started;
    uint32_t erase_ops, erase_skipped;
} yk_writer_t;

/** NULL or TOO_LARGE, FLASH_ERROR. `head` NULL: not head-last. `base` a multiple of YK_INST_BLOCK. */
const char *yk_writer_begin(yk_writer_t *w, const yk_flash_t *flash, uint32_t base, uint32_t size, uint8_t *head,
                            uint8_t *scratch);
/** The stream starts over from byte 0 (a server that ignored a range request): same bytes again, no new erase. */
void yk_writer_restart(yk_writer_t *w);
/** NULL or SIZE_MISMATCH (more than `size`), FLASH_ERROR. */
const char *yk_writer_write(yk_writer_t *w, const uint8_t *data, uint32_t len);
/** After the last byte: NULL when exactly `size` bytes with this SHA-256 were written, else SIZE_MISMATCH or
 *  HASH_MISMATCH. Call once. */
const char *yk_writer_check(yk_writer_t *w, const uint8_t sha256[32]);
/** Head-last: writes the first sector, which makes the slot hold the app. */
const char *yk_writer_commit_head(yk_writer_t *w);
void yk_writer_end(yk_writer_t *w);

/** Reads `size` bytes at `base` back through `flash` (`buf`: YK_INST_SECTOR bytes) and checks their SHA-256: NULL,
 *  FLASH_ERROR or HASH_MISMATCH. Before the slot is written over, the backup is read back with it. */
const char *yk_inst_verify(const yk_flash_t *flash, uint32_t base, uint32_t size, const uint8_t sha256[32], uint8_t *buf);

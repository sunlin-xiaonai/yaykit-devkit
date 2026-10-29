// The recovery's version (P2; docs/delivery/2026-10-25-yay-0.1.5.md 4). Every release's recovery (factory) says
// "0.1.0" in its app description (reproducible builds, no build date), but its ELF sha256 differs from version to
// version: a table of the released ones gives the version, the image's sha256 and its bytes from the description
// alone, without reading 1.6 MB. One the table does not know is hashed once (Yay, in the background, when idle) and
// kept in NVS. Pure C (host tested); reading the partition is in yaykit_app.h (yk_app_recovery_read/hash).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define YK_RECOVERY_ELF_KEY 8  // bytes of the ELF sha256 the table and the NVS record go by
#define YK_RECOVERY_UNKNOWN "unknown"

typedef enum {
    YK_RECOVERY_NONE,     // not read, or the partition holds no image
    YK_RECOVERY_TABLE,    // a release the table knows: only the header and the description were read
    YK_RECOVERY_NVS,      // hashed on an earlier start, kept in NVS (same ELF sha256, same length)
    YK_RECOVERY_HASHED,   // hashed on this start
    YK_RECOVERY_PENDING,  // not in the table and not hashed yet
} yk_recovery_from_t;

typedef struct {
    char version[16];  // "0.1.3"; YK_RECOVERY_UNKNOWN when neither the table nor the description says
    char sha256[65];   // the image's (the package .bin's), 64 lowercase hex; "" not known yet
    uint32_t size;     // image_len (esp_image_get_metadata): the package .bin's bytes
    uint8_t elf[YK_RECOVERY_ELF_KEY];
    yk_recovery_from_t from;
} yk_recovery_t;

/** What the partition says: its description's ELF sha256 (the first YK_RECOVERY_ELF_KEY bytes are enough), its
 *  description's version and image_len. A release the table knows (ELF sha256 and length both match): its version
 *  and sha256, from TABLE. Otherwise PENDING, sha256 "", and the version the description's unless it is "0.1.0" (all
 *  releases so far say that; once the recovery's version follows Yay's package, P3, it will be right), else
 *  YK_RECOVERY_UNKNOWN. */
void yk_recovery_identify(const uint8_t *elf, const char *desc_version, uint32_t image_len, yk_recovery_t *out);

/** The NVS record of a hashed recovery: "1|<ELF sha256 key, 16 hex>|<image_len>|<sha256, 64 hex>". Returns its
 *  length, or -1 when `r` has no 64-hex sha256 or it does not fit. */
int yk_recovery_record(char *out, size_t size, const yk_recovery_t *r);

/** A record (yk_recovery_record) for this recovery (same ELF key, same length): its sha256 goes into `r`, from NVS.
 *  False (and `r` unchanged) for one of another recovery or one not well formed. */
bool yk_recovery_from_record(const char *rec, yk_recovery_t *r);

/** The hash came: `sha256` (64 hex) into `r`, from HASHED. False when it is not 64 lowercase hex. */
bool yk_recovery_hashed(yk_recovery_t *r, const char *sha256);

const char *yk_recovery_from_name(yk_recovery_from_t from);

#ifdef __cplusplus
}
#endif

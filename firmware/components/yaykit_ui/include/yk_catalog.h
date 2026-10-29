// The app catalog cache (plan P23): the names, icons and sizes of the apps in Yay's service list, kept on flash so
// every firmware can draw the app ring without the network. Yay writes it when it is online (from /me's app list);
// every other firmware only reads it. No cache yet (a new device, never online): the ring shows the names' first
// characters.
//
// Where: the first 256 KB of the `userfs` partition (0x1D00000, 3 MB, reserved and mounted by nobody so far). Above
// 16 MB it cannot be mapped, so it is read with esp_partition_read() like `models` and `pkgcache`; nothing in it is
// mapped or executed. The rest of `userfs` stays free (a file system mounted later starts at YK_CATALOG_REGION).
//
// Format v1, little-endian (the pure codec is in yk_apps_logic.c, host tested):
//   offset 0     header, 64 bytes: "YKAC", version 1, count, seq, table CRC-32, written_ms, lang, header CRC-32
//   offset 4096  the table: count entries of 128 bytes (slug, name, version, size, icon format/colour/offset/CRC,
//                channel: added 10-24 in a byte version 1 left reserved, so its older readers see stable; the icon's
//                sha256, its first 8 bytes: added in Yay 0.1.5 in reserved bytes too, to fetch only a changed one)
//   then         the icons: 64 x 64, A8 (4 KB, drawn in the entry's colour) or ARGB8888 (16 KB, LVGL's B, G, R, A)
// The header is written last, after the table and the icons, so a write cut short leaves no catalog rather than a
// broken one. Fields are only ever added (in the reserved bytes, read as zero by version 1 readers).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define YK_CATALOG_MAX 8              // apps in the catalog
#define YK_CATALOG_ICON 64            // icons are 64 x 64
#define YK_CATALOG_REGION (256 * 1024) // the part of `userfs` it may use (8 ARGB icons: 133 KB)
#define YK_CHANNEL_STABLE 0           // yk_catalog_entry_t.channel: the release listed is a stable one ...
#define YK_CHANNEL_PREVIEW 1          // ... or the app's newest preview (it has no stable release yet)
#define YK_CATALOG_ICON_SHA 8         // bytes of an icon's sha256 kept with it

typedef enum {
    YK_ICON_NONE = 0,
    YK_ICON_A8 = 1,        // 4096 bytes of coverage, drawn in `icon_color`
    YK_ICON_ARGB8888 = 2,  // 16384 bytes, B, G, R, A (alpha not premultiplied)
} yk_icon_format_t;

typedef struct {
    char slug[32];        // "breathe"
    char name[48];        // "呼吸之间", in the catalog's language
    char version[16];     // "1.2.0"
    uint32_t size;        // bytes of the slot image ("2.8 MB · 大约二十秒" on the install page)
    uint8_t icon_format;  // yk_icon_format_t
    uint32_t icon_color;  // A8: 0xRRGGBB
    uint8_t channel;      // YK_CHANNEL_STABLE / _PREVIEW (the store's `channel`; read as stable when unknown)
    uint8_t icon_sha[YK_CATALOG_ICON_SHA];  // the icon's sha256 (the store's iconFile), its first bytes; zero: not
                                            // known (written before 0.1.5): fetched again
    // Read back from flash (yk_catalog_write() ignores them):
    uint32_t icon_offset; // in the region
    uint32_t icon_crc;
} yk_catalog_entry_t;

typedef struct {
    int count;
    yk_catalog_entry_t app[YK_CATALOG_MAX];  // in the service list's order
    int64_t written_ms;                      // Unix ms of Yay's write; 0 when the clock was not set
    char lang[8];                            // "zh"
    uint32_t seq;                            // bumped by each write
} yk_catalog_t;

/** Bytes of an icon in this format (0: none or unknown). */
size_t yk_catalog_icon_bytes(uint8_t format);

/** Reads the catalog (header and table; icons stay on flash). ESP_ERR_NOT_FOUND: none or not readable;
 *  ESP_ERR_TIMEOUT: Yay is writing it right now. Anything but ESP_OK: show the first characters. */
esp_err_t yk_catalog_read(yk_catalog_t *out);
/** The entry for a slug, or NULL. */
const yk_catalog_entry_t *yk_catalog_find(const yk_catalog_t *cat, const char *slug);
/** An entry's icon pixels (yk_catalog_icon_bytes() of them) into buf, checked against its CRC.
 *  ESP_ERR_NOT_FOUND: the entry has none; ESP_ERR_INVALID_CRC: it changed or broke on flash. */
esp_err_t yk_catalog_icon(const yk_catalog_entry_t *e, uint8_t *buf, size_t size);

/** Yay only: replaces the catalog. icons[i] are app[i]'s pixels in its format (NULL with YK_ICON_NONE).
 *  The same content as on flash already: nothing is written. Otherwise it erases what it uses (up to 136 KB; the
 *  installer measured about 0.25 s per 64 KB, both cores stalling briefly), so call it from a task, not while
 *  something animates. */
esp_err_t yk_catalog_write(const yk_catalog_t *cat, const uint8_t *const icons[]);
/** Development: forgets the catalog (erases the header). */
esp_err_t yk_catalog_clear(void);

#ifdef __cplusplus
}
#endif

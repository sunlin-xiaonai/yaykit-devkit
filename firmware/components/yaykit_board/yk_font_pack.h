// The `fonts` partition as a pack of fonts (pure C, host tested): one TrueType font per script, the first at offset
// 0 so that firmware which only knows a single font keeps reading it, and an index in the partition's last 64 bytes
// (written by fonts/gen_font_pack.py), little-endian:
//   0  "YKFP"   4  version (1)   5  count (1..4)   6  0, 0
//   8  4 x { lang[4] ASCII, NUL-padded ("zh", "ja"); offset u32; size u32 }
//   56 CRC-32 (IEEE) of bytes 0..55   60  0, 0, 0, 0
// No index (or one that does not hold together): the whole partition is one font, as before.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define YK_FONT_PACK_MAX 4
#define YK_FONT_PACK_TAIL 64

typedef struct {
    char lang[5];
    uint32_t offset, size;
} yk_font_entry_t;

typedef struct {
    int count;
    yk_font_entry_t fonts[YK_FONT_PACK_MAX];
} yk_font_pack_t;

uint32_t yk_crc32(const uint8_t *data, size_t len);

/** Reads the index from a partition's last 64 bytes. False when there is none or it does not hold together (bad
 *  CRC, a font outside the partition or over the index): use the partition as one font. */
bool yk_font_pack_parse(const uint8_t tail[YK_FONT_PACK_TAIL], uint32_t part_size, yk_font_pack_t *out);

/** The order to use the fonts in for a language ("ja", "ja-JP", "zh-Hans", "en"...): the font for that language
 *  first, then the others as the pack lists them (each a fallback for glyphs the ones before it lack). A language
 *  without a font of its own (en, es: Latin is in every font) keeps the pack's order. Returns the count. */
int yk_font_pack_order(const yk_font_pack_t *pack, const char *locale, int order[YK_FONT_PACK_MAX]);

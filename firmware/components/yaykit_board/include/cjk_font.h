/*
 * The UI font: TrueType in the `fonts` flash partition, rendered at runtime by
 * LVGL TinyTTF. The partition is memory-mapped, so the font files never occupy
 * RAM; only the glyph caches do (PSRAM).
 *
 * The partition holds one font (a subset of Noto Sans SC, ../README.md) or a
 * pack of them, one per script (yk_font_pack.h, fonts/gen_font_pack.py): the
 * first still at offset 0, so firmware that reads a single font keeps working.
 * With a pack, the font for the device's language comes first and the others
 * stand behind it for the glyphs it lacks (lv_font_t.fallback).
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

/* Maps the partition and checks it holds a TrueType font (and reads a pack's
 * index). Idempotent. */
esp_err_t cjk_font_init(void);

/* The device's language ("zh", "ja", "ja-JP", "en"...: the owner's, from the
 * account): which font of a pack comes first. Call before the first
 * cjk_font_get(); later it only applies after a restart (fonts in use cannot be
 * swapped). Without a call, or with a single font, the pack's own order. */
void cjk_font_set_language(const char *locale);

/* Font for the given pixel size; created on first use and kept (at most a few
 * sizes are cached). Returns NULL if the font partition is missing or empty.
 * Call with the LVGL lock held. */
const lv_font_t *cjk_font_get(int32_t px);

/* The same for a given language whatever the device's: "ja" draws Han characters
 * the Japanese way, "zh" the Chinese way (Translator shows both on one screen).
 * The others in the pack stand behind it for missing glyphs. Same cache rules;
 * NULL when there is no font. With a single font it is cjk_font_get(). */
const lv_font_t *cjk_font_get_lang(int32_t px, const char *lang);

/* How many sizes stay made at once: one more drops the oldest, and a label still
 * drawing with it crashes. Each language's own font at a size counts as one.
 * Yay made 12 by 0.1.5 and 0.1.6 adds 72 (Settings › 更新): 16 leaves 3 spare
 * (BOSS-2, 10-25). Count before adding a size (`yay fonts`). A slot costs 24
 * bytes of internal RAM; the fonts and their glyph caches are in PSRAM. */
#define CJK_FONT_SIZES 16

/* The sizes made so far, oldest first: their px into `px` (and the pack entry
 * each draws first into `first`, when not NULL), at most `max`. Returns how many
 * there are. For counting what the screens use (development builds). */
int cjk_font_sizes(int32_t *px, int *first, int max);

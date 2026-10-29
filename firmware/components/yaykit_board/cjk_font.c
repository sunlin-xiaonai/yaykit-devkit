#include "cjk_font.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "yk_font_pack.h"

static const char *TAG = "cjk_font";

#define MAX_SIZES CJK_FONT_SIZES /* evicting a size still in use would crash: count them (cjk_font_sizes) */

static const uint8_t *s_data;
static size_t s_size;
static esp_partition_mmap_handle_t s_map;
static yk_font_pack_t s_pack;           /* one entry (the whole partition) when it holds a single font */
static int s_order[YK_FONT_PACK_MAX];   /* the language's font first, then its fallbacks */
static int s_order_n;
static bool s_used;                     /* a size was made: the order is fixed from here */
static char s_lang[8];
static struct {
    int32_t px;
    int first;                          /* the pack entry fonts[0] was made from */
    lv_font_t *fonts[YK_FONT_PACK_MAX]; /* fonts[0] is what callers get; each falls back to the next */
} s_cache[MAX_SIZES];

/* TrueType starts with 0x00010000; an erased partition reads 0xFF. */
static bool is_ttf(const uint8_t *p)
{
    static const uint8_t ttf_magic[4] = { 0x00, 0x01, 0x00, 0x00 };
    return memcmp(p, ttf_magic, sizeof(ttf_magic)) == 0;
}

esp_err_t cjk_font_init(void)
{
    if (s_data) {
        return ESP_OK;
    }
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "fonts");
    if (!part) {
        ESP_LOGE(TAG, "no `fonts` partition (old partition table?)");
        return ESP_ERR_NOT_FOUND;
    }
    const void *ptr = NULL;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, &s_map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap failed: %s", esp_err_to_name(err));
        return err;
    }
    const uint8_t *data = ptr;
    if (!is_ttf(data)) {
        ESP_LOGE(TAG, "`fonts` partition does not hold a TrueType font (flash it with `idf.py flash`)");
        esp_partition_munmap(s_map);
        return ESP_ERR_INVALID_STATE;
    }
    yk_font_pack_t pack;
    if (yk_font_pack_parse(data + part->size - YK_FONT_PACK_TAIL, part->size, &pack)) {
        for (int i = 0; i < pack.count; i++) {
            if (!is_ttf(data + pack.fonts[i].offset)) {
                ESP_LOGW(TAG, "pack font %d (%s) is not TrueType: using the first font only", i, pack.fonts[i].lang);
                pack.count = 0;
                break;
            }
        }
    } else {
        pack.count = 0;
    }
    if (!pack.count) {
        /* One font, as before the pack: the parser only follows table offsets, trailing 0xFF is harmless. */
        pack.count = 1;
        pack.fonts[0] = (yk_font_entry_t){ .lang = "", .offset = 0, .size = part->size };
    }
    s_pack = pack;
    s_data = data;
    s_size = part->size;
    s_order_n = yk_font_pack_order(&s_pack, s_lang, s_order);
    ESP_LOGI(TAG, "mapped %u KB font partition: %d font%s", (unsigned)(part->size / 1024), s_pack.count,
             s_pack.count > 1 ? "s" : "");
    return ESP_OK;
}

void cjk_font_set_language(const char *locale)
{
    if (s_used) {
        ESP_LOGW(TAG, "language %s comes too late (sizes already made); it applies after a restart", locale);
        return;
    }
    snprintf(s_lang, sizeof(s_lang), "%s", locale ? locale : "");
    if (s_data) {
        s_order_n = yk_font_pack_order(&s_pack, s_lang, s_order);
    }
}

static void destroy(int slot)
{
    for (int i = 0; i < YK_FONT_PACK_MAX; i++) {
        if (s_cache[slot].fonts[i]) {
            lv_tiny_ttf_destroy(s_cache[slot].fonts[i]);
        }
    }
    memset(&s_cache[slot], 0, sizeof(s_cache[slot]));
}

/** The font at px whose chain follows `order` (the first entry drawn first, the rest behind it). */
static const lv_font_t *font_for(int32_t px, const int *order, int n)
{
    if (px <= 0 || n <= 0) {
        return NULL;
    }
    for (int i = 0; i < MAX_SIZES; i++) {
        if (s_cache[i].fonts[0] && s_cache[i].px == px && s_cache[i].first == order[0]) {
            return s_cache[i].fonts[0];
        }
    }
    int slot = -1;
    for (int i = 0; i < MAX_SIZES; i++) {
        if (!s_cache[i].fonts[0]) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* All slots taken: replace the oldest size. Screens only use a handful; a label still drawing with it
         * crashes, so say it. */
        ESP_LOGW(TAG, "all %d sizes taken: %d px dropped for %d px", MAX_SIZES, (int)s_cache[0].px, (int)px);
        destroy(0);
        memmove(&s_cache[0], &s_cache[1], sizeof(s_cache[0]) * (MAX_SIZES - 1));
        memset(&s_cache[MAX_SIZES - 1], 0, sizeof(s_cache[0]));
        slot = MAX_SIZES - 1;
    }
    s_used = true;
    /* The language's font, and behind it each other font of the pack for the glyphs it lacks (TinyTTF reports a
     * missing glyph as not found, and LVGL then asks font->fallback). A fallback's glyph cache only fills when
     * it is used. */
    lv_font_t *prev = NULL;
    for (int i = 0; i < n; i++) {
        const yk_font_entry_t *e = &s_pack.fonts[order[i]];
        lv_font_t *font = lv_tiny_ttf_create_data_ex(s_data + e->offset, e->size, px, LV_FONT_KERNING_NONE,
                                                     CONFIG_LV_TINY_TTF_CACHE_GLYPH_CNT);
        if (!font) {
            ESP_LOGE(TAG, "TinyTTF could not open font %d (%s) at %d px", order[i], e->lang, (int)px);
            if (!i) {
                destroy(slot);
                return NULL;
            }
            break;
        }
        s_cache[slot].fonts[i] = font;
        if (prev) {
            prev->fallback = font;
        }
        prev = font;
    }
    s_cache[slot].px = px;
    s_cache[slot].first = order[0];
    return s_cache[slot].fonts[0];
}

int cjk_font_sizes(int32_t *px, int *first, int max)
{
    int n = 0;
    for (int i = 0; i < MAX_SIZES && s_cache[i].fonts[0]; i++, n++) {
        if (n < max) {
            px[n] = s_cache[i].px;
            if (first) {
                first[n] = s_cache[i].first;
            }
        }
    }
    return n;
}

const lv_font_t *cjk_font_get(int32_t px)
{
    if (px <= 0 || cjk_font_init() != ESP_OK) {
        return NULL;
    }
    return font_for(px, s_order, s_order_n);
}

const lv_font_t *cjk_font_get_lang(int32_t px, const char *lang)
{
    if (px <= 0 || cjk_font_init() != ESP_OK) {
        return NULL;
    }
    int order[YK_FONT_PACK_MAX];
    const int n = yk_font_pack_order(&s_pack, lang ? lang : "", order);
    /* A language without a font of its own (en, es) keeps the device's order: the same fonts, no second copy. */
    const bool own = n > 0 && lang && lang[0] && !strncmp(s_pack.fonts[order[0]].lang, lang, 2);
    if (!own || order[0] == s_order[0]) {
        return font_for(px, s_order, s_order_n);
    }
    return font_for(px, order, n);
}

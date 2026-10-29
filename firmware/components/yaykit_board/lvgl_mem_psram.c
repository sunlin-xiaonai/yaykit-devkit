/*
 * LVGL heap in PSRAM (CONFIG_LV_USE_CUSTOM_MALLOC).
 *
 * With the C library allocator every block under 16 KB lands in internal RAM
 * (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL), and LVGL makes many small blocks:
 * widgets, styles, and above all the TinyTTF glyph cache (measured: 25 KB of
 * internal RAM for three lines of Chinese text). Internal RAM is what Wi-Fi,
 * TLS and the audio encoder need, so LVGL is sent to PSRAM instead, falling
 * back to internal RAM only if PSRAM is exhausted. Frame buffers are not
 * affected: the display adapter allocates those itself.
 */
#include <string.h>

#include "esp_heap_caps.h"
#include "lvgl.h"

#define LV_CAPS_PREFERRED (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define LV_CAPS_FALLBACK  (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    (void)mem;
    (void)bytes;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, LV_CAPS_PREFERRED, LV_CAPS_FALLBACK);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return heap_caps_realloc_prefer(p, new_size, 2, LV_CAPS_PREFERRED, LV_CAPS_FALLBACK);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    memset(mon, 0, sizeof(*mon));
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
    mon->total_size        = info.total_free_bytes + info.total_allocated_bytes;
    mon->free_size         = info.total_free_bytes;
    mon->free_biggest_size = info.largest_free_block;
    mon->free_cnt          = info.free_blocks;
    mon->used_cnt          = info.allocated_blocks;
    mon->max_used          = mon->total_size - info.minimum_free_bytes;
    mon->used_pct = mon->total_size ? (uint8_t)(100 * info.total_allocated_bytes / mon->total_size) : 0;
    mon->frag_pct = info.total_free_bytes
                        ? (uint8_t)(100 - (100 * info.largest_free_block / info.total_free_bytes))
                        : 0;
}

lv_result_t lv_mem_test_core(void)
{
    return heap_caps_check_integrity(MALLOC_CAP_SPIRAM, false) ? LV_RESULT_OK : LV_RESULT_INVALID;
}

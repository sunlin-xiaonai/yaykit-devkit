// The app catalog cache (yk_catalog.h) in the first YK_CATALOG_REGION bytes of `userfs`, read and written with
// esp_partition_* (above 16 MB: never mapped). The bytes are yk_catalog_encode()'s (yk_apps_logic.c).
#include "yk_catalog.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "yk_apps_logic.h"

static const char *TAG = "yk_catalog";

#define SECTOR 4096

static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_once = portMUX_INITIALIZER_UNLOCKED;

static const esp_partition_t *part(void) {
    static const esp_partition_t *p;
    if (!p) p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "userfs");
    return p && p->size >= YK_CATALOG_REGION ? p : NULL;
}

/** One reader or writer at a time (Yay writes from its network task while the ring may read). */
static bool lock(void) {
    if (!s_lock) {
        static StaticSemaphore_t buf;
        taskENTER_CRITICAL(&s_once);
        if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&buf);
        taskEXIT_CRITICAL(&s_once);
    }
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(3000)) == pdTRUE;
}

static void unlock(void) { xSemaphoreGive(s_lock); }

/** With the lock: the header and the table. */
static esp_err_t read_locked(const esp_partition_t *p, yk_catalog_t *out) {
    uint8_t h[YK_CATALOG_HEADER];
    uint32_t len, crc;
    esp_err_t err = esp_partition_read(p, 0, h, sizeof(h));
    if (err != ESP_OK) return err;
    if (!yk_catalog_decode_header(h, out, &len, &crc)) return ESP_ERR_NOT_FOUND;
    uint8_t *t = heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM);
    if (!t) return ESP_ERR_NO_MEM;
    err = esp_partition_read(p, YK_CATALOG_TABLE_AT, t, len);
    if (err == ESP_OK && !yk_catalog_decode_table(t, len, crc, out)) err = ESP_ERR_NOT_FOUND;
    heap_caps_free(t);
    return err;
}

esp_err_t yk_catalog_read(yk_catalog_t *out) {
    const esp_partition_t *p = part();
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!p) return ESP_ERR_NOT_FOUND;
    if (!lock()) return ESP_ERR_TIMEOUT;
    const esp_err_t err = read_locked(p, out);
    unlock();
    if (err != ESP_OK) memset(out, 0, sizeof(*out));
    return err == ESP_ERR_NOT_FOUND || err == ESP_OK ? err : ESP_ERR_NOT_FOUND;
}

esp_err_t yk_catalog_icon(const yk_catalog_entry_t *e, uint8_t *buf, size_t size) {
    const esp_partition_t *p = part();
    const size_t bytes = e ? yk_catalog_icon_bytes(e->icon_format) : 0;
    if (!bytes) return ESP_ERR_NOT_FOUND;
    if (!p || !buf || size < bytes) return ESP_ERR_INVALID_ARG;
    if (!lock()) return ESP_ERR_TIMEOUT;
    esp_err_t err = esp_partition_read(p, e->icon_offset, buf, bytes);
    unlock();
    if (err == ESP_OK && yk_catalog_crc32(0, buf, bytes) != e->icon_crc) err = ESP_ERR_INVALID_CRC;
    return err;
}

/** With the lock; img and old are YK_CATALOG_REGION bytes of scratch, two[] two catalogs' (PSRAM: ~2.1 KB together,
 *  not on the caller's stack). */
static esp_err_t write_locked(const esp_partition_t *p, const yk_catalog_t *cat, const uint8_t *const icons[],
                              uint8_t *img, uint8_t *old, yk_catalog_t *two) {
    yk_catalog_t *next = &two[0], *was = &two[1];
    *next = *cat;
    uint32_t len, crc;
    const bool had = esp_partition_read(p, 0, old, YK_CATALOG_HEADER) == ESP_OK &&
                     yk_catalog_decode_header(old, was, &len, &crc);
    if (had) next->seq = was->seq + 1;
    const size_t used = yk_catalog_encode(next, icons, img, YK_CATALOG_REGION);
    if (!used) return ESP_ERR_INVALID_ARG;
    // The same apps, names and icons as on flash: nothing to write (no erase, no wear). The header's sequence
    // number and time do not count; its table CRC and language do.
    if (had && was->count == next->count && crc == yk_catalog_crc32(0, img + YK_CATALOG_TABLE_AT, len) &&
        !strcmp(was->lang, next->lang) && esp_partition_read(p, YK_CATALOG_TABLE_AT, old, used - YK_CATALOG_TABLE_AT) == ESP_OK &&
        !memcmp(old, img + YK_CATALOG_TABLE_AT, used - YK_CATALOG_TABLE_AT)) {
        ESP_LOGI(TAG, "unchanged (%d apps)", next->count);
        return ESP_OK;
    }
    // The table and the icons first, the header last: cut short, it reads as no catalog.
    esp_err_t err = esp_partition_erase_range(p, 0, (used + SECTOR - 1) / SECTOR * SECTOR);
    if (err == ESP_OK && used > YK_CATALOG_TABLE_AT)
        err = esp_partition_write(p, YK_CATALOG_TABLE_AT, img + YK_CATALOG_TABLE_AT, used - YK_CATALOG_TABLE_AT);
    if (err == ESP_OK) err = esp_partition_write(p, 0, img, YK_CATALOG_HEADER);
    ESP_LOGI(TAG, "wrote %d apps, %u bytes (seq %u): %s", next->count, (unsigned)used, (unsigned)next->seq,
             esp_err_to_name(err));
    return err;
}

esp_err_t yk_catalog_write(const yk_catalog_t *cat, const uint8_t *const icons[]) {
    const esp_partition_t *p = part();
    if (!p) return ESP_ERR_NOT_FOUND;
    if (!cat) return ESP_ERR_INVALID_ARG;
    uint8_t *img = heap_caps_malloc(YK_CATALOG_REGION, MALLOC_CAP_SPIRAM);
    uint8_t *old = heap_caps_malloc(YK_CATALOG_REGION, MALLOC_CAP_SPIRAM);
    yk_catalog_t *two = heap_caps_malloc(2 * sizeof(yk_catalog_t), MALLOC_CAP_SPIRAM);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (img && old && two) {
        err = ESP_ERR_TIMEOUT;
        if (lock()) {
            err = write_locked(p, cat, icons, img, old, two);
            unlock();
        }
    }
    heap_caps_free(img);
    heap_caps_free(old);
    heap_caps_free(two);
    return err;
}

esp_err_t yk_catalog_clear(void) {
    const esp_partition_t *p = part();
    if (!p) return ESP_ERR_NOT_FOUND;
    if (!lock()) return ESP_ERR_TIMEOUT;
    const esp_err_t err = esp_partition_erase_range(p, 0, SECTOR);
    unlock();
    return err;
}

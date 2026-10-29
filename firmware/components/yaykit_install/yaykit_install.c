#include "yaykit_install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootloader_common.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_crt_bundle.h"
#include "esp_flash_partitions.h"
#include "esp_freertos_hooks.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "spi_flash_mmap.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "yaykit_app.h"
#include "yaykit_sig.h"
#include "yaykit_slots.h"
#include "yaykit_wifi.h"
#include "yk_install_keys.h"
#include "yk_install_logic.h"

static const char *TAG = "yk_install";

#define CHUNK YK_INST_SECTOR
// esp_http_client mallocs its receive and send buffers. Past CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL they come from PSRAM:
// 5 KB of internal RAM back while it downloads (Yay's line is 4 KB; the recovery's is 16 KB, its buffers stay as
// they were). Only the install task's reads and requests touch them, between flash operations: never with the cache
// off (docs/delivery/2026-10-25-yay-download-memory.md B).
#if CONFIG_SPIRAM_USE_MALLOC && CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL < 8192
#define HTTP_RX_BUFFER (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL + 512)
#define HTTP_TX_BUFFER (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL + 512)
#else
#define HTTP_RX_BUFFER CHUNK
#define HTTP_TX_BUFFER 1024
#endif
#define NS "yk_pkgcache"
#define PROGRESS_US 200000
#if CONFIG_YAY_DEV
#define ALLOW_HTTP true  // a test server on the bench; still signed (the developer key)
#else
#define ALLOW_HTTP false
#endif

static SemaphoreHandle_t s_lock;  // s_status and s_cache
static yk_install_status_t s_status;
static volatile bool s_busy, s_cancel;
static yk_install_listener_t s_listener;
static void *s_listener_ctx;
static int64_t s_last_publish;
static volatile bool s_writing;  // ③, or an app being put back: not cancelled, leaving refused (yk_app_hold_leave)
#if CONFIG_YAY_DEV
static volatile int s_fail_downloads;  // `pkg fail <n>`: download attempts that fail as if the network were gone
#endif

static yk_cache_index_t s_cache;
static const esp_partition_t *s_cache_part;
static bool s_cache_loaded;

static const char *const PHASES[] = {"idle", "downloading", "copying", "verifying", "done", "failed"};
static const char *const STAGES[] = {"fetch", "backup", "write", "restore"};
static const char *const OLDS[] = {"none", "kept", "replaced", "restored", "lost", "pending"};

static void ensure_lock(void) {
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    if (s_lock) return;
    SemaphoreHandle_t lock = xSemaphoreCreateMutex();
    taskENTER_CRITICAL(&mux);
    if (!s_lock) s_lock = lock, lock = NULL;
    taskEXIT_CRITICAL(&mux);
    if (lock) vSemaphoreDelete(lock);
}

static void lock(void) {
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) { xSemaphoreGive(s_lock); }

static void copy(char *out, size_t size, const char *s) { snprintf(out, size, "%s", s ? s : ""); }

// ---------------------------------------------------------------- status

static void publish(void) {
    yk_install_status_t copy;
    yk_install_get_status(&copy);
    s_last_publish = esp_timer_get_time();
    if (s_listener) s_listener(&copy, s_listener_ctx);
}

static void set_phase(yk_install_phase_t phase, const char *error) {
    lock();
    s_status.phase = phase;
    s_status.error = error;
    unlock();
    publish();
}

static void set_done(uint32_t done) {
    lock();
    s_status.done = done;
    unlock();
    if (esp_timer_get_time() - s_last_publish >= PROGRESS_US) publish();
}

/** One install at a time. `from_cache`: it starts as COPYING rather than DOWNLOADING (the ring's first words). */
static bool claim(int slot, const char *slug, const char *version, uint32_t total, bool from_cache,
                  yk_install_listener_t listener, void *ctx) {
    lock();
    const bool ok = !s_busy;
    if (ok) {
        s_busy = true;
        s_cancel = false;
        s_listener = listener;
        s_listener_ctx = ctx;
        memset(&s_status, 0, sizeof(s_status));
        s_status.phase = from_cache ? YK_INSTALL_COPYING : YK_INSTALL_DOWNLOADING;
        s_status.from_cache = from_cache;
        s_status.slot = slot;
        s_status.total = total;
        copy(s_status.slug, sizeof(s_status.slug), slug);
        copy(s_status.version, sizeof(s_status.version), version);
    }
    unlock();
    return ok;
}

static void release(const char *error) {
    if (error) ESP_LOGW(TAG, "install failed: %s", error);
    set_phase(error ? YK_INSTALL_FAILED : YK_INSTALL_DONE, error);
    lock();
    s_busy = false;
    s_listener = NULL;
    unlock();
}

void yk_install_get_status(yk_install_status_t *out) {
    lock();
    *out = s_status;
    unlock();
}

bool yk_install_busy(void) { return s_busy; }

esp_err_t yk_install_cancel(void) {
    lock();  // the lock ③ begins under (begin_writing): one or the other comes first
    yk_inst_flags_t f = {.busy = s_busy, .cancel = s_cancel, .writing = s_writing};
    const bool ok = yk_inst_cancel(&f);
    s_cancel = f.cancel;
    unlock();
    if (!ok && f.writing) printf("YK_INSTALL cancel refused: writing the slot\n");
    return ok ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool yk_install_writing(void) { return s_writing; }

void yk_install_status_json(cJSON *obj) {
    yk_install_status_t s;
    yk_install_get_status(&s);
    cJSON_AddStringToObject(obj, "phase", PHASES[s.phase]);
    if (s.phase == YK_INSTALL_IDLE) return;
    cJSON_AddStringToObject(obj, "stage", STAGES[s.stage]);
    cJSON_AddNumberToObject(obj, "slot", s.slot);
    cJSON_AddStringToObject(obj, "slug", s.slug);
    if (s.version[0]) cJSON_AddStringToObject(obj, "version", s.version);
    cJSON_AddNumberToObject(obj, "done", s.done);
    cJSON_AddNumberToObject(obj, "total", s.total);
    cJSON_AddBoolToObject(obj, "fromCache", s.from_cache);
    if (s.error) cJSON_AddStringToObject(obj, "error", s.error);
    if (s.old == YK_INSTALL_OLD_NONE) return;
    cJSON_AddStringToObject(obj, "old", OLDS[s.old]);
    cJSON_AddStringToObject(obj, "oldSlug", s.old_slug);
}

const char *yk_install_error_text(const char *code, bool en) {
    if (!code) return "";
    static const struct {
        const char *code, *zh, *en;
    } TEXTS[] = {
        {"NO_WIFI", "没连上网络", "No network"},
        {"NOT_FOUND", "找不到这个应用", "App not found"},
        {"DOWNLOAD_FAILED", "下载没成功", "Download failed"},
        {"NOT_CACHED", "要联网下载", "Needs the network"},
        {"NO_FREE_SLOT", "没有空位置", "No free place"},
        {"HOME_SLOT_RESERVED", "位置 1 只放 Yay", "Place 1 is Yay's"},
        {"TOO_LARGE", "应用太大了", "App too large"},
        {"NO_SPACE", "存储空间不够", "Not enough storage"},
        {"UPDATING", "Yay 正在更新，等一会儿再装", "Yay is updating: try again soon"},
        {"BUSY", "正在装别的", "Another install"},
        {"CANCELLED", "已经取消", "Cancelled"},
        {"FLASH_ERROR", "写入没成功", "Could not write"},
        {"NO_MEMORY", "内存不够", "Out of memory"},
        {"LOW_BATTERY", "电量不够，充上电再装", "Battery low: charge first"},
        // BAD_SIGNATURE, SIZE_MISMATCH, HASH_MISMATCH, BAD_IMAGE, WRONG_APP, NOT_HTTPS, BAD_REQUEST: the default.
    };
    for (size_t i = 0; i < sizeof(TEXTS) / sizeof(TEXTS[0]); i++)
        if (!strcmp(code, TEXTS[i].code)) return en ? TEXTS[i].en : TEXTS[i].zh;
    return en ? "Check failed" : "没有通过校验";
}

// ---------------------------------------------------------------- flash

static int part_erase(void *ctx, uint32_t offset, uint32_t len) {
    return esp_partition_erase_range(ctx, offset, len) != ESP_OK;
}

static int part_write(void *ctx, uint32_t offset, const void *data, uint32_t len) {
    return esp_partition_write(ctx, offset, data, len) != ESP_OK;
}

static int part_read(void *ctx, uint32_t offset, void *data, uint32_t len) {
    return esp_partition_read(ctx, offset, data, len) != ESP_OK;
}

// ---------------------------------------------------------------- copies from flash to flash

/* Each read, write and erase holds the other core in spi_flash_op_block_func (its ipc task). Back to back, as in a copy
 * from flash to flash, they left CPU0's idle task no time for seconds (task_wdt) and the screen next to none: whatever
 * is behind there (the screen is, frame after frame) takes every short gap first, a tick between chunks too. So once a
 * core's idle task has not run for COPY_IDLE_US, the copy waits off the flash until it has: everything else on that
 * core has caught up by then. A download waits on the network anyway. */
#define COPY_IDLE_US 150000  // how long a copy may keep a core's idle task out
#define COPY_WAIT_TICKS 50   // the most it then waits for it (something else may never let it run)

static volatile uint32_t s_idle_runs[CONFIG_FREERTOS_NUMBER_OF_CORES];
static bool s_idle_hooked[CONFIG_FREERTOS_NUMBER_OF_CORES];  // each core's own: one that failed is tried again alone

static bool idle_ran0(void) {
    s_idle_runs[0]++;
    return true;
}

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
static bool idle_ran1(void) {
    s_idle_runs[1]++;
    return true;
}
#endif

static const esp_freertos_idle_cb_t IDLE_HOOK[CONFIG_FREERTOS_NUMBER_OF_CORES] = {
    idle_ran0,
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    idle_ran1,
#endif
};

typedef struct {
    bool hooked;  // every core's hook is on: the gate looks at them; else a tick every chunk
    uint32_t seen[CONFIG_FREERTOS_NUMBER_OF_CORES];
    int64_t at;
} copy_gate_t;

static void copy_gate_reset(copy_gate_t *g) {
    for (int i = 0; i < CONFIG_FREERTOS_NUMBER_OF_CORES; i++) g->seen[i] = s_idle_runs[i];
    g->at = esp_timer_get_time();
}

/** A copy begins: the hooks not on yet put on (only those: one on twice would count twice), then from now. */
static void copy_gate_begin(copy_gate_t *g) {
    g->hooked = true;
    for (int i = 0; i < CONFIG_FREERTOS_NUMBER_OF_CORES; i++) {
        if (!s_idle_hooked[i]) s_idle_hooked[i] = esp_register_freertos_idle_hook_for_cpu(IDLE_HOOK[i], i) == ESP_OK;
        g->hooked = g->hooked && s_idle_hooked[i];
    }
    copy_gate_reset(g);
}

static bool idle_all_ran(const copy_gate_t *g) {
    for (int i = 0; i < CONFIG_FREERTOS_NUMBER_OF_CORES; i++)
        if (s_idle_runs[i] == g->seen[i]) return false;
    return true;
}

/** Between two chunks of a copy. */
static void copy_gate(copy_gate_t *g) {
    if (!g->hooked) {
        vTaskDelay(1);  // not every core's hook to look at: a tick off the flash every chunk
        return;
    }
    if (!idle_all_ran(g)) {
        if (esp_timer_get_time() - g->at < COPY_IDLE_US) return;
        for (int i = 0; i < COPY_WAIT_TICKS && !idle_all_ran(g); i++) vTaskDelay(1);
    }
    copy_gate_reset(g);
}

static yk_flash_t flash_of(const esp_partition_t *part) {
    return (yk_flash_t){.erase = part_erase, .write = part_write, .read = part_read, .ctx = (void *)part,
                        .capacity = part->size};
}

static const esp_partition_t *slot_partition(int slot) {
    if (slot < 0 || slot >= YK_SLOT_COUNT) return NULL;
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN + slot, NULL);
}

/** Whether an image of `size` bytes may go into `slot` now. */
static const char *slot_check(int slot, uint32_t size) {
    const esp_partition_t *part = slot_partition(slot);
    if (!part) return "NO_SUCH_SLOT";
    if (size > part->size) return "TOO_LARGE";
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running && running->address == part->address) return "SLOT_IN_USE";
    return NULL;
}

/** From here on the slot no longer holds its old app. */
static const char *slot_begin(yk_writer_t *w, int slot, uint32_t size, uint8_t *head, uint8_t *scratch) {
    const char *error = slot_check(slot, size);
    if (error) return error;
    const yk_flash_t flash = flash_of(slot_partition(slot));
    return yk_writer_begin(w, &flash, 0, size, head, scratch);
}

/**
 * The image just written must not inherit the old one's rollback verdict (Yay would show it as broken): forget the
 * otadata entry that names this slot. The boot partition stays as it is, except in the recovery when it named this
 * very slot (it then becomes the recovery itself, as the launcher always did).
 */
static void fresh_verdict(const esp_partition_t *part) {
    const esp_partition_t *otadata =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    const int count = esp_ota_get_app_partition_count();
    if (!otadata || !count) return;
    const int slot = part->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
    for (int round = 0; round < 2; round++) {
        esp_ota_select_entry_t e[2];
        for (int i = 0; i < 2; i++)
            if (esp_partition_read(otadata, i * SPI_FLASH_SEC_SIZE, &e[i], sizeof(e[i])) != ESP_OK) return;
        const int active = bootloader_common_get_active_otadata(e);
        if (active >= 0 && (int)((e[active].ota_seq - 1) % count) == slot) {
            // Booting this slot was asked for and the restart never came: boot where this firmware runs instead.
            if (round || esp_ota_set_boot_partition(esp_ota_get_running_partition()) != ESP_OK) return;
            continue;
        }
        for (int i = 0; i < 2; i++)
            if (i != active && bootloader_common_ota_select_valid(&e[i]) && (int)((e[i].ota_seq - 1) % count) == slot)
                esp_partition_erase_range(otadata, i * SPI_FLASH_SEC_SIZE, SPI_FLASH_SEC_SIZE);
        return;
    }
}

/** After the last byte: the hash, the image's header and project name; then the first sector, and the bootloader's
 *  own check of what is on flash. */
static const char *slot_finish(yk_writer_t *w, int slot, const uint8_t sha256[32], const char *slug) {
    const char *error = yk_writer_check(w, sha256);
    if (error) return error;
    const esp_app_desc_t *desc =
        (const esp_app_desc_t *)(w->head + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (w->head[0] != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD) return "BAD_IMAGE";
    char project[sizeof(desc->project_name) + 1];
    memcpy(project, desc->project_name, sizeof(desc->project_name));
    project[sizeof(desc->project_name)] = 0;
    if (!strcmp(project, "yaykit_launcher")) return "WRONG_APP";
    if (slug && *slug && !yk_inst_project_matches(project, slug)) return "WRONG_APP";
    // Whatever the request said: 位置 1 takes Yay only, and Yay goes nowhere else. The slot stays empty.
    if ((error = yk_inst_slot_rule_project(slot, project))) return error;
    if ((error = yk_writer_commit_head(w))) return error;
    const esp_partition_t *part = slot_partition(slot);
    esp_image_metadata_t meta;
    const esp_partition_pos_t pos = {.offset = part->address, .size = part->size};
    if (esp_image_verify(ESP_IMAGE_VERIFY, &pos, &meta) != ESP_OK) {
        esp_partition_erase_range(part, 0, YK_INST_SECTOR);
        return "BAD_IMAGE";
    }
    fresh_verdict(part);
    return NULL;
}

// ---------------------------------------------------------------- package cache

static void to_hex(const uint8_t in[32], char out[65]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) out[2 * i] = digits[in[i] >> 4], out[2 * i + 1] = digits[in[i] & 15];
    out[64] = 0;
}

static void cache_load(void) {
    if (s_cache_loaded) return;
    s_cache_loaded = true;
    memset(&s_cache, 0, sizeof(s_cache));
    s_cache_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "pkgcache");
    if (!s_cache_part) return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = YK_CACHE_BLOB_MAX;
    uint8_t *blob = malloc(len);
    if (blob && nvs_get_blob(h, "index", blob, &len) == ESP_OK &&
        !yk_cache_decode(blob, len, s_cache_part->size, &s_cache))
        ESP_LOGW(TAG, "package cache index unreadable; starting empty");
    free(blob);
    nvs_close(h);
}

/** With the lock held. */
static void cache_save(void) {
    uint8_t *blob = malloc(YK_CACHE_BLOB_MAX);
    nvs_handle_t h;
    if (!blob) return;
    const size_t len = yk_cache_encode(&s_cache, blob, YK_CACHE_BLOB_MAX);
    if (len && nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_blob(h, "index", blob, len) != ESP_OK || nvs_commit(h) != ESP_OK)
            ESP_LOGW(TAG, "package cache index not saved");
        nvs_close(h);
    }
    free(blob);
}

esp_err_t yk_install_init(void) {
    lock();
    cache_load();
    const int count = s_cache.count;
    unlock();
    ESP_LOGI(TAG, "package cache: %s, %d image(s)", s_cache_part ? "ready" : "no partition", count);
    return ESP_OK;
}

/** This exact image in the cache: a copy of its entry. */
static bool cache_lookup(const yk_install_manifest_t *m, yk_cache_entry_t *out) {
    lock();
    cache_load();
    const int i = s_cache_part ? yk_cache_find(&s_cache, m->slug, m->sha256) : -1;
    if (i >= 0) *out = s_cache.e[i];
    unlock();
    return i >= 0;
}

/** The entry holding exactly this image (any signature, a backup too), or false. */
static bool cache_lookup_image(const char *slug, const char *sha256, yk_cache_entry_t *out) {
    lock();
    cache_load();
    const int i = s_cache_part ? yk_cache_find(&s_cache, slug, sha256) : -1;
    if (i >= 0) *out = s_cache.e[i];
    unlock();
    return i >= 0;
}

/** A signed copy of exactly this image (not a backup without a signature): it can come back any time. */
// yk_install_plan()'s helpers are not inlined (0.1.4: yay_run overflowed its 5 KB): each one's buffers, an app's
// description and its note (~0.7 KB), a cache entry (~0.3 KB), are on the stack only while it runs, not all at once.
static __attribute__((noinline)) bool cache_signed_copy(const char *slug, const char *sha256) {
    yk_cache_entry_t e;
    return cache_lookup_image(slug, sha256, &e) && !yk_cache_is_backup(&e);
}

/** A backup without a signature goes once its app is safe (installed over, or back in its slot). */
static void cache_drop_backup(const char *slug, const char *sha256) {
    lock();
    const int i = yk_cache_find(&s_cache, slug, sha256);
    if (i >= 0 && yk_cache_is_backup(&s_cache.e[i])) {
        yk_cache_remove(&s_cache, i);
        cache_save();
    }
    unlock();
}

static void cache_drop_image(const char *slug, const char *sha256) {
    lock();
    const int i = yk_cache_find(&s_cache, slug, sha256);
    if (i >= 0) {
        yk_cache_remove(&s_cache, i);
        cache_save();
    }
    unlock();
}

static void cache_touch(const char *slug, const char *sha256) {
    lock();
    const int i = yk_cache_find(&s_cache, slug, sha256);
    if (i >= 0) {
        yk_cache_touch(&s_cache, i);
        cache_save();
    }
    unlock();
}

/** What the main firmware's update keeps in the cache (yk_app_update.h): its new image and the previous version, from
 *  the request until the new Yay is confirmed; nothing otherwise. `u` holds the strings. Before lock(): reads NVS. */
static yk_cache_keep_t update_keep(yk_update_t *u) {
    if (yk_app_update_get(u) != ESP_OK || !yk_update_holds_cache(u)) return (yk_cache_keep_t){0};
    return (yk_cache_keep_t){.sha256 = {u->sha256}, .slug = YK_INSTALL_PREVIOUS};
}

// ---------------------------------------------------------------- the replacement's record (yk_replace_t)

#define REPLACE_KEY "replace"

static bool replace_load(yk_replace_t *r) {
    nvs_handle_t h;
    char text[YK_REPLACE_TEXT_MAX];
    size_t len = sizeof(text);
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    const bool got = nvs_get_str(h, REPLACE_KEY, text, &len) == ESP_OK;
    nvs_close(h);
    if (got && yk_replace_parse(text, r)) return true;
    if (got) ESP_LOGW(TAG, "replacement record unreadable; dropped");
    return false;
}

static bool replace_save(const yk_replace_t *r) {
    char text[YK_REPLACE_TEXT_MAX];
    nvs_handle_t h;
    if (yk_replace_format(text, sizeof(text), r) < 0 || nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_str(h, REPLACE_KEY, text) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void replace_clear(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_erase_key(h, REPLACE_KEY) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------- what this component wrote into a slot

/* NVS "slot1", "slot2": "<slug>|<version>|<the build's ELF SHA-256>|<the image's SHA-256>", written once an image is in
 * the slot, cleared before the slot is written (a USB install too). yk_install_plan knows from it which image a slot
 * holds without reading 3 MB: the header's slug, version and ELF hash must still be the ones noted. */

static void note_key(int slot, char key[8]) { snprintf(key, 8, "slot%d", slot); }

static void slot_note_clear(int slot) {
    nvs_handle_t h;
    char key[8];
    note_key(slot, key);
    if (slot == YK_INST_HOME_SLOT || nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_erase_key(h, key) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

static void slot_note_save(int slot, const char *slug, const char *version, const char *sha256) {
    const esp_partition_t *part = slot_partition(slot);
    esp_app_desc_t desc;
    nvs_handle_t h;
    char key[8], elf[65], text[200];
    if (slot == YK_INST_HOME_SLOT || !part || esp_ota_get_partition_description(part, &desc) != ESP_OK) return;
    to_hex(desc.app_elf_sha256, elf);
    snprintf(text, sizeof(text), "%s|%s|%s|%s", slug, version, elf, sha256);
    note_key(slot, key);
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, key, text) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

/** The noted image's SHA-256 into `sha256` when the slot still holds that build of `slug`. */
static __attribute__((noinline)) bool slot_note_sha(int slot, const char *slug, char sha256[65]) {
    const esp_partition_t *part = slot_partition(slot);
    esp_app_desc_t desc;
    nvs_handle_t h;
    char key[8], text[200], want[200], elf[65];
    size_t len = sizeof(text);
    sha256[0] = 0;
    if (slot == YK_INST_HOME_SLOT || !part || esp_ota_get_partition_description(part, &desc) != ESP_OK) return false;
    note_key(slot, key);
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    const bool got = nvs_get_str(h, key, text, &len) == ESP_OK;
    nvs_close(h);
    to_hex(desc.app_elf_sha256, elf);
    const int n = snprintf(want, sizeof(want), "%s|%.32s|%s|", slug, desc.version, elf);
    if (!got || n <= 0 || strncmp(text, want, n) || strlen(text + n) != 64) return false;
    copy(sha256, 65, text + n);
    return true;
}

int yk_install_cache_list(yk_install_cached_t *out, int max) {
    lock();
    cache_load();
    int order[YK_INST_CACHE_MAX], n = 0;
    const int all = yk_cache_order(&s_cache, order, YK_INST_CACHE_MAX);
    for (int k = 0; k < all && n < max; k++) {
        const yk_cache_entry_t *e = &s_cache.e[order[k]];
        if (yk_cache_is_backup(e)) continue;  // only to put its app back, never offered
        memset(&out[n], 0, sizeof(out[n]));
        copy(out[n].slug, sizeof(out[n].slug), e->slug);
        copy(out[n].name, sizeof(out[n].name), e->name);
        copy(out[n].version, sizeof(out[n].version), e->version);
        out[n].size = e->size;
        n++;
    }
    unlock();
    return n;
}

esp_err_t yk_install_cache_manifest(const char *slug, yk_install_manifest_t *out) {
    lock();
    cache_load();
    int i = -1;  // the newest signed one (a backup without a signature is never installed this way)
    for (int k = 0; slug && k < s_cache.count; k++)
        if (!strcmp(s_cache.e[k].slug, slug) && !yk_cache_is_backup(&s_cache.e[k]) && (i < 0 || s_cache.e[k].seq > s_cache.e[i].seq))
            i = k;
    if (i >= 0) {
        const yk_cache_entry_t *e = &s_cache.e[i];
        memset(out, 0, sizeof(*out));
        copy(out->slug, sizeof(out->slug), e->slug);
        copy(out->name, sizeof(out->name), e->name);
        copy(out->version, sizeof(out->version), e->version);
        copy(out->sha256, sizeof(out->sha256), e->sha256);
        copy(out->signature, sizeof(out->signature), e->signature);
        out->size = e->size;
    }
    unlock();
    return i >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t yk_install_cache_forget(const char *slug) {
    yk_update_t u;
    const yk_cache_keep_t keep = update_keep(&u);
    int kept = 0;
    lock();
    cache_load();
    for (int i = s_cache.count - 1; i >= 0; i--)
        if (!slug || !strcmp(s_cache.e[i].slug, slug)) {
            if (yk_cache_kept(&s_cache.e[i], &keep)) kept++;
            else yk_cache_remove(&s_cache, i);
        }
    cache_save();
    unlock();
    if (kept) ESP_LOGW(TAG, "package cache: %d image(s) kept for the update going", kept);
    return ESP_OK;
}

void yk_install_cache_json(cJSON *obj) {
    lock();
    cache_load();
    uint32_t used = 0;
    for (int i = 0; i < s_cache.count; i++) used += s_cache.e[i].size;
    cJSON_AddNumberToObject(obj, "capacity", s_cache_part ? s_cache_part->size : 0);
    cJSON_AddNumberToObject(obj, "used", used);
    cJSON *items = cJSON_AddArrayToObject(obj, "items");
    int order[YK_INST_CACHE_MAX];
    const int n = yk_cache_order(&s_cache, order, YK_INST_CACHE_MAX);
    for (int k = 0; k < n; k++) {
        const yk_cache_entry_t *e = &s_cache.e[order[k]];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "slug", e->slug);
        if (e->name[0]) cJSON_AddStringToObject(item, "name", e->name);
        cJSON_AddStringToObject(item, "version", e->version);
        cJSON_AddNumberToObject(item, "size", e->size);
        cJSON_AddNumberToObject(item, "offset", e->offset);
        if (yk_cache_is_backup(e)) cJSON_AddBoolToObject(item, "backup", true);  // only to put its app back
        cJSON_AddItemToArray(items, item);
    }
    unlock();
}

// ---------------------------------------------------------------- manifests

const char *yk_install_check(const yk_install_manifest_t *m) {
    const char *error = yk_inst_check_fields(m->slug, m->version, m->size, m->sha256, m->url, m->signature, ALLOW_HTTP);
    if (error) return error;
    size_t count;
    const uint8_t (*keys)[YK_SIG_KEY_BYTES] = yk_install_keys(&count);
    const yk_sig_image_t image = {.slug = m->slug, .version = m->version, .size = m->size, .sha256 = m->sha256,
                                  .board = YK_INSTALL_BOARD, .layout = YK_INSTALL_LAYOUT};
    const yk_sig_result_t verdict = yk_sig_verify(&image, m->signature, keys, count);
    if (verdict == YK_SIG_OK) return NULL;
    printf("YK_INSTALL_SIGNATURE %s slug=%s version=%s\n", yk_sig_result_name(verdict), m->slug, m->version);
    return "BAD_SIGNATURE";
}

static bool str_field(const cJSON *obj, const char *name, char *out, size_t size, bool required) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!v || cJSON_IsNull(v)) return !required;
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= size) return false;
    strcpy(out, v->valuestring);
    return true;
}

const char *yk_install_from_json(const cJSON *obj, yk_install_manifest_t *m) {
    memset(m, 0, sizeof(*m));
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(obj, "size");
    if (!cJSON_IsObject(obj) || !str_field(obj, "slug", m->slug, sizeof(m->slug), true) ||
        !str_field(obj, "name", m->name, sizeof(m->name), false) ||
        !str_field(obj, "version", m->version, sizeof(m->version), true) ||
        !str_field(obj, "sha256", m->sha256, sizeof(m->sha256), true) ||
        !str_field(obj, "url", m->url, sizeof(m->url), false) ||
        !str_field(obj, "signature", m->signature, sizeof(m->signature), true) || !cJSON_IsNumber(size) ||
        size->valuedouble < 1 || size->valuedouble > YK_INST_MAX_IMAGE || size->valuedouble != (uint32_t)size->valuedouble)
        return "BAD_REQUEST";
    m->size = (uint32_t)size->valuedouble;
    return NULL;
}

static __attribute__((noinline)) void read_slots(yk_inst_slot_t slots[YK_INST_SLOTS]) {
    for (int i = 0; i < YK_INST_SLOTS; i++) {
        yk_slot_t s;
        memset(&slots[i], 0, sizeof(slots[i]));
        if (yk_slot_read(i, &s) != ESP_OK) continue;
        slots[i].present = s.present;
        slots[i].running = s.running;
        slots[i].broken = s.present && (!strcmp(s.state, "invalid") || !strcmp(s.state, "aborted"));
        copy(slots[i].slug, sizeof(slots[i].slug), s.slug);
        slots[i].used = yk_app_last_used(i);
    }
}

/** The cache has this very image (no copy of its entry left on the caller's stack). */
static __attribute__((noinline)) bool cache_has(const yk_install_manifest_t *m) {
    yk_cache_entry_t hit;
    return cache_lookup(m, &hit);
}

void yk_install_plan(const yk_install_manifest_t *m, const char *replace, yk_install_plan_t *out) {
    yk_inst_slot_t slots[YK_INST_SLOTS];
    read_slots(slots);
    yk_inst_choice_t choice;
    yk_inst_choose(slots, m->slug, replace, &choice);
    memset(out, 0, sizeof(*out));
    out->slot = choice.slot;
    out->update = choice.update;
    out->replaces = choice.replaces;
    copy(out->replace, sizeof(out->replace), choice.replace);
    out->error = choice.error;
    out->cached = cache_has(m);
    yk_slot_t s;
    if ((out->update || out->replaces) && yk_slot_read(out->slot, &s) == ESP_OK) {
        copy(out->replace_version, sizeof(out->replace_version), s.version);
        out->replace_signed = slot_note_sha(out->slot, s.slug, out->replace_sha256) &&
                              cache_signed_copy(s.slug, out->replace_sha256);
    }
}

// ---------------------------------------------------------------- installing

typedef struct {
    yk_install_manifest_t m;
    int slot;
    uint8_t sha256[32];
    yk_writer_t to_slot, to_cache;
    bool begun;    // the slot is being written: its old app is gone
    bool caching;  // to_cache is begun
    uint32_t cache_offset;
    uint8_t *head, *scratch, *buf;
    yk_install_manifest_t replaced;  // the store's release of the app the slot holds (yk_install_start_replacing)
    bool has_replaced;
    yk_cache_keep_t keep;  // what stays in the cache while this runs: the update's, the new image, the backup
    uint32_t base;         // bytes of the stages before this one: the progress runs on through all of them
    yk_replace_t rec;      // yk_install_resume: the record it acts on, and how
    yk_replace_step_t resume;
    yk_cache_entry_t backup;
    uint32_t beside;       // yk_install_fetch: bytes that must still fit beside the image
    yk_fetch_room_t room;  // ... and what may go for their room
    char in_slot[3][32];   // YK_FETCH_ROOM_APPS_LAST: the apps in the slots (their copies go last)
} job_t;

/** One chunk of the download into the cache (①): stopped by a cancel, which loses only this download. */
static const char *feed_cache(job_t *j, const uint8_t *data, uint32_t len) {
    const char *error = yk_writer_write(&j->to_cache, data, len);
    set_done(j->base + j->to_cache.written);
    return error ? error : s_cancel ? "CANCELLED" : NULL;
}

/** One chunk into the slot (③, or an app put back): the slot is only touched once the first bytes are here. Never
 *  cancelled. */
static const char *feed_slot(job_t *j, const uint8_t *data, uint32_t len) {
    if (!j->begun) {
        const char *error = slot_begin(&j->to_slot, j->slot, j->m.size, j->head, j->scratch);
        if (error) return error;
        j->begun = true;
    }
    const char *error = yk_writer_write(&j->to_slot, data, len);
    set_done(j->base + j->to_slot.written);
    return error;
}

/** The entries `before` had that `after` has not, on the console: what went for room. */
static void log_dropped(const yk_cache_index_t *before, const yk_cache_index_t *after, const yk_cache_keep_t *keep) {
    for (int i = 0; i < before->count; i++) {
        if (yk_cache_find(after, before->e[i].slug, before->e[i].sha256) >= 0) continue;
        bool slot = false;
        for (int k = 0; keep && k < 3; k++) slot |= keep->last[k] && !strcmp(keep->last[k], before->e[i].slug);
        printf("YK_INSTALL_CACHE drop slug=%s version=%s size=%u slot=%d\n", before->e[i].slug, before->e[i].version,
               (unsigned)before->e[i].size, slot);
    }
}

/** Room in the cache for this image while it downloads (①), beside what j->keep holds. */
static const char *cache_reserve(job_t *j) {
    lock();
    cache_load();
    int64_t at = YK_CACHE_NEVER;
    if (s_cache_part) {
        int dropped = 0;
        const yk_cache_index_t before = s_cache;
        at = yk_cache_place(&s_cache, j->m.size, s_cache_part->size, &j->keep, NULL, &dropped);
        if (dropped) {
            log_dropped(&before, &s_cache, &j->keep);
            cache_save();  // what is about to be overwritten is no longer listed
        }
    }
    unlock();
    if (at < 0) return at == YK_CACHE_NEVER ? "TOO_LARGE" : j->keep.slug ? "UPDATING" : "NO_SPACE";
    const yk_flash_t flash = flash_of(s_cache_part);
    const char *error = yk_writer_begin(&j->to_cache, &flash, (uint32_t)at, j->m.size, NULL, j->scratch);
    if (error) return error;
    j->caching = true;
    j->cache_offset = (uint32_t)at;
    return NULL;
}

/** The downloaded image listed (its bytes checked): `out` gets its entry. Older images of the app stay until the slot
 *  holds this one (cache_drop_older). */
static void cache_commit(job_t *j, yk_cache_entry_t *out) {
    yk_cache_entry_t e = {.size = j->m.size, .offset = j->cache_offset};
    copy(e.slug, sizeof(e.slug), j->m.slug);
    copy(e.name, sizeof(e.name), j->m.name);
    copy(e.version, sizeof(e.version), j->m.version);
    copy(e.sha256, sizeof(e.sha256), j->m.sha256);
    copy(e.signature, sizeof(e.signature), j->m.signature);
    lock();
    yk_cache_add(&s_cache, &e);
    *out = s_cache.e[s_cache.count - 1];
    cache_save();
    unlock();
}

/** Installed: the app's older images go, this one replaces them (not the update's). */
static void cache_drop_older(const char *slug, const char *sha256) {
    yk_update_t u;
    const yk_cache_keep_t keep = update_keep(&u);
    lock();
    for (int i = s_cache.count - 1; i >= 0; i--)
        if (!strcmp(s_cache.e[i].slug, slug) && strcmp(s_cache.e[i].sha256, sha256) && !yk_cache_kept(&s_cache.e[i], &keep))
            yk_cache_remove(&s_cache, i);
    cache_save();
    unlock();
}

static bool is_redirect(int code) { return code == 301 || code == 302 || code == 303 || code == 307 || code == 308; }

/** `pkg fail <n>` (development builds): this attempt fails as if the network were gone. */
static bool fail_injected(void) {
#if CONFIG_YAY_DEV
    if (s_fail_downloads > 0) {
        s_fail_downloads--;
        printf("YK_INSTALL_FAIL injected, %d left\n", s_fail_downloads);
        return true;
    }
#endif
    return false;
}

/** Downloads into the cache (①); resumes with a range request after a dropped connection. */
static const char *download(job_t *j) {
    if (!yk_wifi_wait_connected(20000)) return "NO_WIFI";
    esp_http_client_config_t cfg = {
        .url = j->m.url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = HTTP_RX_BUFFER,
        .buffer_size_tx = HTTP_TX_BUFFER,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    const char *error = client ? NULL : "NO_MEMORY";
    int failures = 0;
    while (!error && j->to_cache.written < j->m.size) {
        char range[32];
        if (j->to_cache.written) {
            snprintf(range, sizeof(range), "bytes=%u-", (unsigned)j->to_cache.written);
            esp_http_client_set_header(client, "Range", range);
        } else {
            esp_http_client_delete_header(client, "Range");
        }
        int code = 0;
        const bool failed = fail_injected();
        for (int hops = 0; !failed && hops < 5; hops++) {
            if (esp_http_client_open(client, 0) != ESP_OK) break;
            esp_http_client_fetch_headers(client);
            code = esp_http_client_get_status_code(client);
            if (!is_redirect(code)) break;
            while (esp_http_client_read(client, (char *)j->buf, CHUNK) > 0) {}
            esp_http_client_close(client);
            if (esp_http_client_set_redirection(client) != ESP_OK) break;
            code = 0;
        }
        if (code == 404 || code == 403 || code == 410) {
            error = "NOT_FOUND";
            break;
        }
        if (code == 200 && j->to_cache.written) yk_writer_restart(&j->to_cache);  // the range ignored: from the start
        if (code == 200 || code == 206) {
            int n;
            while (!error && (n = esp_http_client_read(client, (char *)j->buf, CHUNK)) > 0) error = feed_cache(j, j->buf, n);
        }
        esp_http_client_close(client);
        if (!error && j->to_cache.written < j->m.size) {
            if (++failures > 5) {
                error = "DOWNLOAD_FAILED";
            } else {
                ESP_LOGW(TAG, "download interrupted at %u (HTTP %d), retrying", (unsigned)j->to_cache.written, code);
                vTaskDelay(pdMS_TO_TICKS(1000 * failures));
                if (!yk_wifi_wait_connected(30000)) error = "NO_WIFI";
                if (!error && s_cancel) error = "CANCELLED";
            }
        }
    }
    if (client) esp_http_client_cleanup(client);
    return error;
}

static const char *copy_from_cache(job_t *j, const yk_cache_entry_t *e) {
    const char *error = NULL;
    copy_gate_t gate;
    copy_gate_begin(&gate);
    for (uint32_t at = 0; !error && at < e->size; at += CHUNK) {
        const uint32_t n = e->size - at < CHUNK ? e->size - at : CHUNK;
        if (esp_partition_read(s_cache_part, e->offset + at, j->buf, n) != ESP_OK) return "FLASH_ERROR";
        error = feed_slot(j, j->buf, n);
        copy_gate(&gate);
    }
    return error;
}

/** The slot written from the cache entry `e`, then checked as `slug`'s image with this SHA-256. */
static const char *write_from_cache(job_t *j, const yk_cache_entry_t *e, const uint8_t sha256[32], const char *slug) {
    const char *error = copy_from_cache(j, e);
    if (!error) {
        set_phase(YK_INSTALL_VERIFYING, NULL);
        error = slot_finish(&j->to_slot, j->slot, sha256, slug);
    }
    if (j->begun) yk_writer_end(&j->to_slot);
    j->begun = false;
    return error;
}

/** What went wrong could be the cached copy rather than the image: drop it and download instead. */
static bool cache_was_bad(const char *error) {
    return !strcmp(error, "FLASH_ERROR") || !strcmp(error, "SIZE_MISMATCH") || !strcmp(error, "HASH_MISMATCH") ||
           !strcmp(error, "BAD_IMAGE") || !strcmp(error, "WRONG_APP");
}

static void set_from_cache(bool from_cache) {
    lock();
    s_status.from_cache = from_cache;
    unlock();
}

static void set_stage(job_t *j, yk_install_stage_t stage, yk_install_phase_t phase) {
    lock();
    s_status.stage = stage;
    unlock();
    printf("YK_INSTALL_STAGE %s slot=%d slug=%s done=%u\n", STAGES[stage], j->slot, j->m.slug, (unsigned)j->base);
    set_phase(phase, NULL);
}

static void set_total(uint32_t total) {
    lock();
    s_status.total = total;
    unlock();
}

static void set_old(yk_install_old_t old, const char *slug, bool signed_copy) {
    lock();
    s_status.old = old;
    copy(s_status.old_slug, sizeof(s_status.old_slug), slug);
    s_status.old_signed = signed_copy;
    unlock();
}

/** ③ begins under the lock a cancel takes too (yk_install_cancel): one that came first stops it with the slot
 *  untouched, any later is refused. From here leaving is refused (BUSY) until end_writing(). */
static bool begin_writing(void) {
    lock();
    yk_inst_flags_t f = {.busy = s_busy, .cancel = s_cancel, .writing = s_writing};
    const bool go = yk_inst_begin_write(&f);
    s_writing = f.writing;
    if (go) yk_app_hold_leave(true);
    unlock();
    return go;
}

static void end_writing(void) {
    lock();
    s_writing = false;
    yk_app_hold_leave(false);
    unlock();
}

/** The app in a slot now: what a replacement keeps until the new one is in. */
typedef struct {
    bool present;  // a valid image (a broken one is not kept)
    char slug[32], version[32], sha256[65];
    uint32_t len;
} slot_app_t;

static const char *slot_image(int slot, uint32_t *len, esp_app_desc_t *desc);

static void project_slug(const char *project, char *out, size_t size) {
    snprintf(out, size, "%.31s", strncmp(project, "yaykit_", 7) ? project : project + 7);
    for (char *c = out; *c; c++)
        if (*c == '_') *c = '-';
}

/** What `slot` holds, with the SHA-256 of its image read from flash (a fraction of a second for 3 MB). Never 位置 1:
 *  Yay's update keeps its own previous version (yk_app_update.h). An app that is there but can not be read whole, or
 *  whose name is no slug, is not written over (it could not be kept): FLASH_ERROR, WRONG_APP. */
static const char *slot_app(int slot, slot_app_t *out, uint8_t *buf) {
    memset(out, 0, sizeof(*out));
    esp_app_desc_t desc;
    if (slot == YK_INST_HOME_SLOT || slot_image(slot, &out->len, &desc)) return NULL;  // empty, or nothing to keep
    project_slug(desc.project_name, out->slug, sizeof(out->slug));
    copy(out->version, sizeof(out->version), desc.version);
    if (!yk_inst_slug_ok(out->slug)) return "WRONG_APP";
    const esp_partition_t *part = slot_partition(slot);
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    bool ok = true;
    for (uint32_t at = 0; ok && at < out->len; at += CHUNK) {
        const uint32_t n = out->len - at < CHUNK ? out->len - at : CHUNK;
        ok = esp_partition_read(part, at, buf, n) == ESP_OK;
        if (ok) mbedtls_sha256_update(&sha, buf, n);
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (!ok) return "FLASH_ERROR";
    to_hex(digest, out->sha256);
    out->present = true;
    return NULL;
}

/** Reads a cached image back and checks its SHA-256: NULL, FLASH_ERROR or HASH_MISMATCH. */
static const char *cache_verify(const yk_cache_entry_t *e, uint8_t *buf) {
    uint8_t want[32];
    if (!s_cache_part || !yk_inst_hex32(e->sha256, want)) return "HASH_MISMATCH";
    const yk_flash_t flash = flash_of(s_cache_part);
    return yk_inst_verify(&flash, e->offset, e->size, want, buf);
}

/** Whether an image of `first` bytes and then a backup of `second` fit beside what j->keep holds (asked before ①). */
static const char *room_for(job_t *j, uint32_t first, uint32_t second) {
    lock();
    cache_load();
    const int fit = s_cache_part ? yk_cache_fits(&s_cache, first, second, s_cache_part->size, &j->keep) : YK_CACHE_NEVER;
    unlock();
    if (!fit) return NULL;
    return fit == YK_CACHE_NEVER ? "TOO_LARGE" : j->keep.slug ? "UPDATING" : "NO_SPACE";
}

/**
 * ②: the app the slot holds into the cache beside the new image (both kept). Its signature when the store's release
 * of it is these very bytes and that signature checks out (it can then come back any time), else
 * YK_CACHE_BACKUP_SIGNATURE: only to put it back. Stopped by a cancel: the slot is not touched.
 */
static const char *backup_slot(job_t *j, const slot_app_t *old, yk_cache_entry_t *out) {
    const bool released = j->has_replaced && !strcmp(j->replaced.slug, old->slug) &&
                          !strcmp(j->replaced.sha256, old->sha256) && j->replaced.size == old->len &&
                          !yk_install_check(&j->replaced);
    lock();
    cache_load();
    int dropped = 0;
    const int64_t at = yk_cache_place(&s_cache, old->len, s_cache_part->size, &j->keep, NULL, &dropped);
    if (dropped) cache_save();
    unlock();
    if (at < 0) return at == YK_CACHE_NEVER ? "TOO_LARGE" : j->keep.slug ? "UPDATING" : "NO_SPACE";
    const esp_partition_t *part = slot_partition(j->slot);
    const yk_flash_t flash = flash_of(s_cache_part);
    yk_writer_t w;
    const char *error = yk_writer_begin(&w, &flash, (uint32_t)at, old->len, NULL, j->scratch);
    const bool begun = !error;
    copy_gate_t gate;
    copy_gate_begin(&gate);
    for (uint32_t done = 0; !error && done < old->len; done += CHUNK) {
        const uint32_t n = old->len - done < CHUNK ? old->len - done : CHUNK;
        if (esp_partition_read(part, done, j->buf, n) != ESP_OK) error = "FLASH_ERROR";
        else error = yk_writer_write(&w, j->buf, n);
        set_done(j->base + done + n);
        if (!error && s_cancel) error = "CANCELLED";
        copy_gate(&gate);
    }
    uint8_t digest[32];
    if (!error) error = yk_inst_hex32(old->sha256, digest) ? yk_writer_check(&w, digest) : "HASH_MISMATCH";
    if (begun) yk_writer_end(&w);
    if (!error) {
        yk_cache_entry_t e = {.size = old->len, .offset = (uint32_t)at};
        copy(e.slug, sizeof(e.slug), old->slug);
        copy(e.name, sizeof(e.name), released ? j->replaced.name : "");
        copy(e.version, sizeof(e.version), old->version);
        copy(e.sha256, sizeof(e.sha256), old->sha256);
        copy(e.signature, sizeof(e.signature), released ? j->replaced.signature : YK_CACHE_BACKUP_SIGNATURE);
        lock();
        yk_cache_add(&s_cache, &e);
        *out = s_cache.e[s_cache.count - 1];
        cache_save();
        unlock();
    }
    printf("YK_INSTALL_CACHE backup=%s slot=%d slug=%s version=%s bytes=%u signed=%d\n", error ? error : "stored",
           j->slot, old->slug, old->version, (unsigned)old->len, released);
    return error;
}

/**
 * The replaced app back into its slot from its backup (③ failed, or at start after a power cut). The record says so
 * first, so that a cut now is tried again at the next start (YK_REPLACE_TRIES in all). Done: the backup without a
 * signature goes, then the record. Failed now: the record stays for the next start while the backup reads back right
 * and tries are left (PENDING), else both go (LOST). Under begin_writing().
 */
static yk_install_old_t put_back(job_t *j, yk_replace_t *rec, const yk_cache_entry_t *backup) {
    const int64_t started = esp_timer_get_time();
    rec->state = YK_REPLACE_RESTORING;
    rec->tries++;
    const bool saved = replace_save(rec);
    if (!saved) {  // its count would not go up and a cut would try forever: this one try, and no record
        printf("YK_INSTALL_RESTORE record not saved: one try, no more\n");
        replace_clear();
    }
    yk_install_status_t st;
    yk_install_get_status(&st);
    j->base = st.done;
    set_total(st.done + backup->size);
    const yk_install_manifest_t installing = j->m;  // the slot is written for the old app's size and name now
    copy(j->m.slug, sizeof(j->m.slug), rec->old_slug);
    j->m.size = backup->size;
    lock();
    copy(s_status.slug, sizeof(s_status.slug), rec->old_slug);
    copy(s_status.version, sizeof(s_status.version), backup->version);
    unlock();
    set_stage(j, YK_INSTALL_STAGE_RESTORE, YK_INSTALL_COPYING);
    uint8_t digest[32];
    // No project name to match: the SHA-256 shows it is the very image that was there.
    const char *error =
        yk_inst_hex32(rec->old_sha256, digest) ? write_from_cache(j, backup, digest, NULL) : "HASH_MISMATCH";
    j->m = installing;
    yk_install_old_t became = YK_INSTALL_OLD_RESTORED;
    if (!error) {
        slot_note_save(j->slot, rec->old_slug, backup->version, rec->old_sha256);
        cache_drop_backup(rec->old_slug, rec->old_sha256);  // first the backup, then the record (a cut between: CLEAR)
        replace_clear();
    } else {
        const bool backup_ok = !cache_verify(backup, j->buf);
        if (saved && yk_replace_retry(rec, backup_ok)) {
            became = YK_INSTALL_OLD_PENDING;  // the record stays: the next start puts it back
        } else {
            became = YK_INSTALL_OLD_LOST;
            if (backup_ok) cache_drop_backup(rec->old_slug, rec->old_sha256);
            else cache_drop_image(rec->old_slug, rec->old_sha256);
            replace_clear();
        }
    }
    printf("YK_INSTALL_RESTORE %s slot=%d slug=%s version=%s bytes=%u try=%u ms=%d old=%s\n", error ? error : "OK",
           j->slot, rec->old_slug, backup->version, (unsigned)backup->size, (unsigned)rec->tries,
           (int)((esp_timer_get_time() - started) / 1000), OLDS[became]);
    return became;
}

static void install_task(void *arg) {
    job_t *j = arg;
    const int64_t started = esp_timer_get_time();
    const char *from = "net", *cache = "skip";
    const char *error = yk_install_check(&j->m);  // in this task: ECDSA wants ~3 KB of stack
    // What the slot holds now: kept until the new app is in (②), and the room for both asked for before anything.
    slot_app_t old;
    if (!error) error = slot_app(j->slot, &old, j->buf);
    else memset(&old, 0, sizeof(old));
    yk_update_t u;
    j->keep = update_keep(&u);
    yk_cache_entry_t hit = {0}, backup = {0};
    bool have = !error && cache_lookup(&j->m, &hit);
    if (have && (error = cache_verify(&hit, j->buf))) {  // a bad copy: dropped, downloaded instead
        printf("YK_INSTALL_CACHE bad=%s slug=%s\n", error, j->m.slug);
        cache_drop_image(j->m.slug, j->m.sha256);
        have = false;
        cache = "bad";
        error = NULL;
    }
    bool backed = old.present && cache_lookup_image(old.slug, old.sha256, &backup);
    if (old.present) {
        yk_cache_keep_add(&j->keep, old.sha256);
        slot_note_save(j->slot, old.slug, old.version, old.sha256);  // what it holds, known now
        set_old(YK_INSTALL_OLD_KEPT, old.slug, backed && !yk_cache_is_backup(&backup));
    }
    if (have) yk_cache_keep_add(&j->keep, j->m.sha256);
    uint32_t backup_len = old.present && !backed ? old.len : 0;
    if (!error) error = room_for(j, have ? 0 : j->m.size, backup_len);
    if (!error) set_total((have ? 0 : j->m.size) + backup_len + j->m.size);
    // ① The new image into the cache, whole and checked (or found there).
    if (!error && have) {
        from = "cache";
        cache = "hit";
        set_from_cache(true);
    } else if (!error && !j->m.url[0]) {
        error = "NOT_CACHED";
    } else if (!error) {
        set_from_cache(false);
        set_stage(j, YK_INSTALL_STAGE_FETCH, YK_INSTALL_DOWNLOADING);
        error = cache_reserve(j);
        if (!error) error = download(j);
        if (!error) error = yk_writer_check(&j->to_cache, j->sha256);
        if (j->caching) yk_writer_end(&j->to_cache);
        j->caching = false;
        if (!error) {
            cache_commit(j, &hit);
            cache = "stored";
            have = true;
            yk_cache_keep_add(&j->keep, j->m.sha256);
        }
        j->base += j->m.size;
        // The same image as the slot's (installed again): the copy just made is its backup too, not a second one.
        if (!error && old.present && !backed && (backed = cache_lookup_image(old.slug, old.sha256, &backup)))
            set_total(j->base + j->m.size);
    }
    // ② The app the slot holds, when the cache has no copy of it; and whichever copy it is, read back before the slot
    //    is written over: a bad one is dropped and made again (once).
    for (int round = 0; !error && old.present && round < 2; round++) {
        if (!backed) {
            set_stage(j, YK_INSTALL_STAGE_BACKUP, YK_INSTALL_COPYING);
            error = backup_slot(j, &old, &backup);
            j->base += old.len;
            if (error) break;
            backed = true;
        }
        const char *bad = cache_verify(&backup, j->buf);
        if (!bad) break;
        printf("YK_INSTALL_CACHE backup_bad=%s slug=%s round=%d\n", bad, old.slug, round);
        cache_drop_image(old.slug, old.sha256);
        backed = false;
        set_total(j->base + old.len + j->m.size);
        if (round) error = bad;  // made again and still bad: the slot is not touched
    }
    if (!error && old.present) set_old(YK_INSTALL_OLD_KEPT, old.slug, !yk_cache_is_backup(&backup));
    // ③ The slot, from the cache: not cancelled, leaving refused. Should it fail, the old app goes back at once; a power
    //   cut leaves the record for the next start (yk_install_resume).
    yk_install_old_t became = old.present ? YK_INSTALL_OLD_KEPT : YK_INSTALL_OLD_NONE;
    if (!error) {
        yk_replace_t rec = {.state = YK_REPLACE_WRITING, .slot = j->slot};
        copy(rec.new_slug, sizeof(rec.new_slug), j->m.slug);
        copy(rec.new_sha256, sizeof(rec.new_sha256), j->m.sha256);
        copy(rec.old_slug, sizeof(rec.old_slug), old.slug);
        copy(rec.old_sha256, sizeof(rec.old_sha256), old.sha256);
        if (!begin_writing()) {
            error = "CANCELLED";  // asked just before: the slot is untouched
        } else if (old.present && !replace_save(&rec)) {
            error = "FLASH_ERROR";  // no record, no writing over the app
        } else {
            slot_note_clear(j->slot);
            set_stage(j, YK_INSTALL_STAGE_WRITE, YK_INSTALL_COPYING);
            error = write_from_cache(j, &hit, j->sha256, j->m.slug);
            if (!error) {
                became = old.present ? YK_INSTALL_OLD_REPLACED : YK_INSTALL_OLD_NONE;
                slot_note_save(j->slot, j->m.slug, j->m.version, j->m.sha256);
                cache_touch(j->m.slug, j->m.sha256);
                cache_drop_older(j->m.slug, j->m.sha256);
                if (old.present) {
                    cache_drop_backup(old.slug, old.sha256);  // first the backup, then the record
                    replace_clear();
                }
            } else if (old.present) {
                printf("YK_INSTALL %s writing slot=%d: %s goes back\n", error, j->slot, old.slug);
                if (cache_was_bad(error) && strcmp(error, "FLASH_ERROR")) cache_drop_image(j->m.slug, j->m.sha256);
                became = put_back(j, &rec, &backup);
            }
        }
        end_writing();
    }
    if (old.present) set_old(became, old.slug, cache_signed_copy(old.slug, old.sha256));
    printf("YK_INSTALL %s slot=%d slug=%s version=%s bytes=%u ms=%d from=%s cache=%s erased=%u skipped=%u old=%s%s%s "
           "stack_left=%u\n",
           error ? error : "OK", j->slot, j->m.slug, j->m.version, (unsigned)j->m.size,
           (int)((esp_timer_get_time() - started) / 1000), from, cache, (unsigned)j->to_slot.erase_ops,
           (unsigned)j->to_slot.erase_skipped, OLDS[became], old.present ? ":" : "", old.slug,
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
    free(j->head);
    free(j->scratch);
    free(j->buf);
    free(j);
    release(error);
    vTaskDelete(NULL);
}

/** Buffers flash reads land in stay in internal RAM (no bounce buffer per read); the kept first sector may be in
 *  PSRAM. */
static uint8_t *internal_buffer(void) {
    uint8_t *p = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : malloc(CHUNK);
}

/** A job with its buffers, or NULL. */
/** In PSRAM (2.3 KB of internal RAM back, B): only the install task's code reads it, never a flash operation (those
 *  get j->buf and j->scratch, internal, and j->head, which esp_flash copies from in small pieces with the cache on). */
static job_t *job_alloc(void) {
    job_t *j = heap_caps_calloc(1, sizeof(*j), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return j ? j : calloc(1, sizeof(*j));
}

static job_t *job_new(void) {
    job_t *j = job_alloc();
    if (!j) return NULL;
    j->head = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!j->head) j->head = malloc(CHUNK);
    j->scratch = internal_buffer();
    j->buf = internal_buffer();
    if (j->head && j->scratch && j->buf) return j;
    free(j->head), free(j->scratch), free(j->buf), free(j);
    return NULL;
}

esp_err_t yk_install_start(const yk_install_manifest_t *m, int slot, yk_install_listener_t listener, void *ctx) {
    return yk_install_start_replacing(m, slot, NULL, listener, ctx);
}

esp_err_t yk_install_start_replacing(const yk_install_manifest_t *m, int slot, const yk_install_manifest_t *replaced,
                                     yk_install_listener_t listener, void *ctx) {
    yk_cache_entry_t hit;
    if (!claim(slot, m->slug, m->version, m->size, cache_lookup(m, &hit), listener, ctx)) return ESP_ERR_INVALID_STATE;
    const char *error =
        yk_inst_check_fields(m->slug, m->version, m->size, m->sha256, m->url, m->signature, ALLOW_HTTP);
    // The main firmware only in slot 0 (位置 1), apps only in the others.
    if (!error) error = yk_inst_slot_rule(slot, m->slug);
    if (!error) error = slot_check(slot, m->size);
    // An app waiting to be put back (its record kept for the next start): one record only, nothing over it.
    yk_replace_t pending;
    if (!error && replace_load(&pending)) {
        printf("YK_INSTALL BUSY: %s waits to go back into slot %d\n", pending.old_slug, pending.slot + 1);
        error = "BUSY";
    }
    job_t *j = error ? NULL : job_new();
    if (!error && !j) error = "NO_MEMORY";
    if (j) {
        j->m = *m;
        j->slot = slot;
        yk_inst_hex32(m->sha256, j->sha256);
        if (replaced) j->replaced = *replaced, j->has_replaced = true;
    }
    // Flash writes need an internal-RAM stack; TLS and ECDSA run on it too.
    if (!error && xTaskCreate(install_task, "install", 8192, j, 4, NULL) != pdPASS) error = "NO_MEMORY";
    if (error) {
        if (j) free(j->head), free(j->scratch), free(j->buf), free(j);
        printf("YK_INSTALL %s slot=%d slug=%s\n", error, slot, m->slug);
        release(error);
        return !strcmp(error, "NO_MEMORY") ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    }
    publish();
    return ESP_OK;
}

// ---------------------------------------------------------------- into the cache only (yk_install_fetch)

/** What an image fetched into the cache may push out for its room: anything the rest does not hold (a tap, as an
 *  install), only its own slug's other images and the previous Yay (YK_FETCH_ROOM_OWN), or anything with the copies of
 *  the apps in the slots last (YK_FETCH_ROOM_APPS_LAST; their slugs into `in_slot`, which `keep` points at). */
static void room_keep(yk_cache_keep_t *keep, const char *slug, yk_fetch_room_t room, char in_slot[3][32]) {
    if (room == YK_FETCH_ROOM_OWN) {
        keep->only[0] = slug;
        keep->only[1] = YK_INSTALL_PREVIOUS;
    } else if (room == YK_FETCH_ROOM_APPS_LAST) {
        for (int i = 0; i < 3; i++) {
            yk_slot_t s;
            in_slot[i][0] = 0;
            if (yk_slot_read(i, &s) == ESP_OK && s.present) snprintf(in_slot[i], 32, "%s", s.slug);
            keep->last[i] = in_slot[i][0] ? in_slot[i] : NULL;
        }
    }
}

static void fetch_room(job_t *j) { room_keep(&j->keep, j->m.slug, j->room, j->in_slot); }


/** The cached image read back and hashed, a 1 ms pause after each chunk (no idle task starved, #555). */
static const char *cache_verify_paced(const yk_cache_entry_t *e, uint8_t *buf) {
    uint8_t want[32];
    if (!s_cache_part || !yk_inst_hex32(e->sha256, want)) return "HASH_MISMATCH";
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    const char *error = NULL;
    for (uint32_t at = 0; !error && at < e->size; at += CHUNK) {
        const uint32_t n = e->size - at < CHUNK ? e->size - at : CHUNK;
        if (esp_partition_read(s_cache_part, e->offset + at, buf, n) != ESP_OK) error = "FLASH_ERROR";
        else mbedtls_sha256_update(&sha, buf, n);
        vTaskDelay(1);
    }
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    return error ? error : memcmp(digest, want, 32) ? "HASH_MISMATCH" : NULL;
}

/** Stage ① alone: the image checked, into the cache (or found there), no slot written. */
static void fetch_task(void *arg) {
    job_t *j = arg;
    const int64_t started = esp_timer_get_time();
    const char *cache = "miss";
    const char *error = yk_install_check(&j->m);  // in this task: ECDSA wants ~3 KB of stack
    yk_update_t u;
    j->keep = update_keep(&u);
    fetch_room(j);
    yk_cache_entry_t hit = {0};
    bool have = !error && cache_lookup(&j->m, &hit);
    if (have && (error = cache_verify_paced(&hit, j->buf))) {  // a bad copy: dropped, downloaded instead
        printf("YK_INSTALL_CACHE bad=%s slug=%s\n", error, j->m.slug);
        cache_drop_image(j->m.slug, j->m.sha256);
        have = false;
        cache = "bad";
        error = NULL;
    }
    if (have) {
        cache = "hit";
        yk_cache_keep_add(&j->keep, j->m.sha256);
    }
    // Room for it and for what must fit beside it, asked before anything comes (nothing dropped by asking).
    if (!error) error = room_for(j, have ? 0 : j->m.size, j->beside);
    if (!error && !have && !j->m.url[0]) error = "NOT_CACHED";
    if (!error && !have) {
        set_total(j->m.size);
        set_from_cache(false);
        set_stage(j, YK_INSTALL_STAGE_FETCH, YK_INSTALL_DOWNLOADING);
        error = cache_reserve(j);
        if (!error) error = download(j);
        if (!error) error = yk_writer_check(&j->to_cache, j->sha256);
        if (j->caching) yk_writer_end(&j->to_cache);
        j->caching = false;
        if (!error) {
            cache_commit(j, &hit);
            cache = "stored";
        }
    }
    printf("YK_INSTALL_FETCH %s slug=%s version=%s bytes=%u ms=%d cache=%s room=%d stack_left=%u\n",
           error ? error : "OK", j->m.slug, j->m.version, (unsigned)j->m.size,
           (int)((esp_timer_get_time() - started) / 1000), cache, (int)j->room,
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
    free(j->head);
    free(j->scratch);
    free(j->buf);
    free(j);
    release(error);
    vTaskDelete(NULL);
}

esp_err_t yk_install_fetch(const yk_install_manifest_t *m, uint32_t beside, bool make_room,
                           yk_install_listener_t listener, void *ctx) {
    return yk_install_fetch_room(m, beside, make_room ? YK_FETCH_ROOM_ANY : YK_FETCH_ROOM_OWN, listener, ctx);
}

esp_err_t yk_install_fetch_room(const yk_install_manifest_t *m, uint32_t beside, yk_fetch_room_t room,
                                yk_install_listener_t listener, void *ctx) {
    yk_cache_entry_t hit;
    if (!claim(-1, m->slug, m->version, m->size, cache_lookup(m, &hit), listener, ctx)) return ESP_ERR_INVALID_STATE;
    const char *error =
        yk_inst_check_fields(m->slug, m->version, m->size, m->sha256, m->url, m->signature, ALLOW_HTTP);
    if (!error && m->size > YK_INST_MAX_IMAGE) error = "TOO_LARGE";
    // An app waiting to be put back (its record kept for the next start): nothing beside it, as for an install.
    yk_replace_t pending;
    if (!error && replace_load(&pending)) {
        printf("YK_INSTALL BUSY: %s waits to go back into slot %d\n", pending.old_slug, pending.slot + 1);
        error = "BUSY";
    }
    job_t *j = error ? NULL : job_new();
    if (!error && !j) error = "NO_MEMORY";
    if (j) {
        j->m = *m;
        j->slot = -1;
        j->beside = beside;
        j->room = room;
        yk_inst_hex32(m->sha256, j->sha256);
    }
    // TLS and ECDSA on an internal-RAM stack, as an install's.
    if (!error && xTaskCreate(fetch_task, "install", 8192, j, 4, NULL) != pdPASS) error = "NO_MEMORY";
    if (error) {
        if (j) free(j->head), free(j->scratch), free(j->buf), free(j);
        printf("YK_INSTALL_FETCH %s slug=%s\n", error, m->slug);
        release(error);
        return !strcmp(error, "NO_MEMORY") ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    }
    publish();
    return ESP_OK;
}

const char *yk_install_cache_check(const char *slug, const char *sha256, uint32_t beside, bool make_room) {
    return yk_install_cache_check_room(slug, sha256, beside, make_room ? YK_FETCH_ROOM_ANY : YK_FETCH_ROOM_OWN);
}

const char *yk_install_cache_check_room(const char *slug, const char *sha256, uint32_t beside, yk_fetch_room_t room) {
    yk_cache_entry_t e;
    if (!slug || !sha256 || !cache_lookup_image(slug, sha256, &e) || yk_cache_is_backup(&e)) return "NOT_CACHED";
    uint8_t *buf = internal_buffer();
    if (!buf) return "NO_MEMORY";
    const int64_t started = esp_timer_get_time();
    const char *error = cache_verify_paced(&e, buf);
    free(buf);
    if (error && strcmp(error, "FLASH_ERROR")) cache_drop_image(slug, sha256);  // a bad copy: downloaded again
    if (!error && beside) {
        yk_update_t u;
        yk_cache_keep_t keep = update_keep(&u);
        yk_cache_keep_add(&keep, e.sha256);
        char in_slot[3][32];
        room_keep(&keep, slug, room, in_slot);
        lock();
        cache_load();
        const int fit = s_cache_part ? yk_cache_fits(&s_cache, 0, beside, s_cache_part->size, &keep) : YK_CACHE_NEVER;
        unlock();
        if (fit) error = fit == YK_CACHE_NEVER ? "TOO_LARGE" : "NO_SPACE";
    }
    printf("YK_INSTALL_CACHE check=%s slug=%s bytes=%u beside=%u ms=%d\n", error ? error : "OK", slug, (unsigned)e.size,
           (unsigned)beside, (int)((esp_timer_get_time() - started) / 1000));
    return error;
}

// ---------------------------------------------------------------- after a power cut (yk_install_resume)

static void resume_task(void *arg) {
    job_t *j = arg;
    const char *error = NULL;
    yk_install_old_t became;
    if (j->resume == YK_REPLACE_LOST) {
        error = j->backup.size ? "RESTORE_FAILED" : "NOT_CACHED";
        printf("YK_INSTALL_RESTORE %s slot=%d slug=%s tries=%u: given up\n", error, j->slot, j->rec.old_slug,
               (unsigned)j->rec.tries);
        cache_drop_backup(j->rec.old_slug, j->rec.old_sha256);  // first the backup, then the record
        replace_clear();
        became = YK_INSTALL_OLD_LOST;
    } else {
        begin_writing();  // nothing to cancel it: it was claimed just now
        became = put_back(j, &j->rec, &j->backup);
        end_writing();
        if (became != YK_INSTALL_OLD_RESTORED) error = "RESTORE_FAILED";
    }
    set_old(became, j->rec.old_slug, false);
    free(j->head);
    free(j->scratch);
    free(j->buf);
    free(j);
    release(error);
    vTaskDelete(NULL);
}

/** The record of a replacement cut short and what to do with it (false: none). yk_install_resume() and
 *  yk_install_resume_pending() both decide with this. */
static bool resume_plan(yk_replace_t *rec, yk_replace_in_t *in, yk_cache_entry_t *backup, yk_replace_step_t *step) {
    if (!replace_load(rec)) return false;
    *in = (yk_replace_in_t){.has = true, .rec = *rec};
    uint32_t len = 0;
    esp_app_desc_t desc;
    in->slot_valid = !slot_image(rec->slot, &len, &desc);
    *backup = (yk_cache_entry_t){0};
    in->backup_cached = cache_lookup_image(rec->old_slug, rec->old_sha256, backup);
    *step = yk_replace_next(in);
    return true;
}

bool yk_install_resume_pending(void) {
    yk_replace_t rec;
    yk_replace_in_t in;
    yk_cache_entry_t backup;
    yk_replace_step_t step;
    return resume_plan(&rec, &in, &backup, &step) && step != YK_REPLACE_GO && step != YK_REPLACE_CLEAR;
}

esp_err_t yk_install_resume(yk_install_listener_t listener, void *ctx) {
    yk_replace_t rec;
    yk_replace_in_t in;
    yk_cache_entry_t backup;
    yk_replace_step_t step;
    if (!resume_plan(&rec, &in, &backup, &step)) return ESP_ERR_NOT_FOUND;
    printf("YK_INSTALL_RESUME step=%s slot=%d new=%s old=%s tries=%u slot_valid=%d backup=%d\n",
           yk_replace_step_name(step), rec.slot, rec.new_slug, rec.old_slug, (unsigned)rec.tries, in.slot_valid,
           in.backup_cached);
    if (step == YK_REPLACE_GO || step == YK_REPLACE_CLEAR) {
        cache_drop_backup(rec.old_slug, rec.old_sha256);  // first the backup, then the record
        replace_clear();
        return ESP_ERR_NOT_FOUND;
    }
    if (!claim(rec.slot, rec.old_slug, backup.version, backup.size, true, listener, ctx)) return ESP_ERR_INVALID_STATE;
    lock();
    s_status.stage = YK_INSTALL_STAGE_RESTORE;
    s_status.old = YK_INSTALL_OLD_KEPT;
    copy(s_status.old_slug, sizeof(s_status.old_slug), rec.old_slug);
    unlock();
    job_t *j = job_new();
    if (j) {
        j->slot = rec.slot;
        j->rec = rec;
        j->resume = step;
        j->backup = backup;
    }
    if (!j || xTaskCreate(resume_task, "install", 8192, j, 4, NULL) != pdPASS) {
        if (j) free(j->head), free(j->scratch), free(j->buf), free(j);
        set_old(YK_INSTALL_OLD_PENDING, rec.old_slug, false);  // the record stays: the next start puts it back
        release("NO_MEMORY");
        return ESP_OK;
    }
    publish();
    return ESP_OK;
}

// ---------------------------------------------------------------- the main firmware's update (yk_app_update.h)

/** The image in `slot`: how long it is (the bootloader's own check) and its description. */
static const char *slot_image(int slot, uint32_t *len, esp_app_desc_t *desc) {
    const esp_partition_t *part = slot_partition(slot);
    if (!part) return "NO_SUCH_SLOT";
    esp_image_metadata_t meta;
    const esp_partition_pos_t pos = {.offset = part->address, .size = part->size};
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK ||
        esp_ota_get_partition_description(part, desc) != ESP_OK)
        return "BAD_IMAGE";
    *len = meta.image_len;
    return *len >= YK_INST_SECTOR && *len <= YK_INST_MAX_IMAGE ? NULL : "BAD_IMAGE";
}

/** The newest cache entry of `slug` (copied), or false. */
static bool cache_newest(const char *slug, yk_cache_entry_t *out) {
    lock();
    cache_load();
    const int i = s_cache_part ? yk_cache_newest(&s_cache, slug) : -1;
    if (i >= 0) *out = s_cache.e[i];
    unlock();
    return i >= 0;
}

/**
 * Copies the image in `slot` into the package cache as `slug` (its older copy goes), with `signature` as its entry's.
 * The cache's least recently used images make room, never the update's new image (update_keep); NO_SPACE when they
 * leave too little, and then nothing is dropped. Shows as COPYING in yk_install_get_status. Blocks: a few seconds for
 * a 3 MB image.
 */
static const char *slot_to_cache(int slot, const char *slug, const char *signature) {
    uint32_t len = 0;
    esp_app_desc_t desc;
    const char *error = slot_image(slot, &len, &desc);
    if (error) return error;
    yk_update_t u;
    const yk_cache_keep_t keep = update_keep(&u);
    if (!claim(slot, slug, desc.version, len, true, NULL, NULL)) return "BUSY";
    lock();
    cache_load();
    int64_t at = YK_CACHE_NEVER;
    if (s_cache_part) {
        int dropped = 0;
        at = yk_cache_place(&s_cache, len, s_cache_part->size, &keep, slug, &dropped);
        if (dropped) cache_save();  // what is about to be overwritten is no longer listed
    }
    unlock();
    if (at < 0) error = !s_cache_part ? "FLASH_ERROR" : at == YK_CACHE_FULL ? "NO_SPACE" : "TOO_LARGE";
    const esp_partition_t *part = slot_partition(slot);
    uint8_t *scratch = error ? NULL : internal_buffer(), *buf = error ? NULL : internal_buffer();
    if (!error && (!scratch || !buf)) error = "NO_MEMORY";
    yk_writer_t w;
    const yk_flash_t flash = s_cache_part ? flash_of(s_cache_part) : (yk_flash_t){0};
    const bool begun = !error && !(error = yk_writer_begin(&w, &flash, (uint32_t)at, len, NULL, scratch));
    mbedtls_sha256_context sha;
    uint8_t digest[32];
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    copy_gate_t gate;
    copy_gate_begin(&gate);
    for (uint32_t done = 0; !error && done < len; done += CHUNK) {
        const uint32_t n = len - done < CHUNK ? len - done : CHUNK;
        if (esp_partition_read(part, done, buf, n) != ESP_OK) error = "FLASH_ERROR";
        else mbedtls_sha256_update(&sha, buf, n), error = yk_writer_write(&w, buf, n);
        set_done(done + n);
        copy_gate(&gate);
    }
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (!error) error = yk_writer_check(&w, digest);
    if (begun) yk_writer_end(&w);
    free(scratch);
    free(buf);
    if (!error) {
        yk_cache_entry_t e = {.size = len, .offset = (uint32_t)at};
        copy(e.slug, sizeof(e.slug), slug);
        copy(e.version, sizeof(e.version), desc.version);
        copy(e.signature, sizeof(e.signature), signature);
        to_hex(digest, e.sha256);
        lock();
        yk_cache_add(&s_cache, &e);
        cache_save();
        unlock();
    }
    printf("YK_INSTALL_CACHE %s slot=%d as=%s version=%s bytes=%u\n", error ? error : "stored", slot, slug, desc.version,
           (unsigned)len);
    release(error);
    return error;
}

const char *yk_install_backup_home(void) {
    return slot_to_cache(YK_INST_HOME_SLOT, YK_INSTALL_PREVIOUS, YK_INSTALL_PREVIOUS_SIGNATURE);
}

const char *yk_install_restore_home(void) {
    yk_cache_entry_t e;
    if (!cache_newest(YK_INSTALL_PREVIOUS, &e)) return "NOT_CACHED";
    if (!claim(YK_INST_HOME_SLOT, YK_APP_HOME_SLUG, e.version, e.size, true, NULL, NULL)) return "BUSY";
    const int64_t started = esp_timer_get_time();
    job_t *j = job_alloc();
    const char *error = j ? slot_check(YK_INST_HOME_SLOT, e.size) : "NO_MEMORY";
    if (!error) {
        copy(j->m.slug, sizeof(j->m.slug), YK_APP_HOME_SLUG);
        copy(j->m.version, sizeof(j->m.version), e.version);
        j->m.size = e.size;
        j->slot = YK_INST_HOME_SLOT;
        yk_inst_hex32(e.sha256, j->sha256);
        j->head = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!j->head) j->head = malloc(CHUNK);
        j->scratch = internal_buffer();
        j->buf = internal_buffer();
        if (!j->head || !j->scratch || !j->buf) error = "NO_MEMORY";
    }
    // No signature: it is the copy of a Yay that ran in this slot. Its SHA-256 and the image are checked.
    if (!error) error = copy_from_cache(j, &e);
    if (!error) {
        set_phase(YK_INSTALL_VERIFYING, NULL);
        error = slot_finish(&j->to_slot, YK_INST_HOME_SLOT, j->sha256, YK_APP_HOME_SLUG);
    }
    if (j && j->begun) yk_writer_end(&j->to_slot);
    if (error && cache_was_bad(error)) cache_drop_image(YK_INSTALL_PREVIOUS, e.sha256);
    printf("YK_INSTALL %s slot=0 slug=yay version=%s bytes=%u ms=%d from=previous\n", error ? error : "OK", e.version,
           (unsigned)e.size, (int)((esp_timer_get_time() - started) / 1000));
    if (j) free(j->head), free(j->scratch), free(j->buf), free(j);
    release(error);
    return error;
}

bool yk_install_previous(char *version, size_t size) {
    yk_cache_entry_t e;
    const bool found = cache_newest(YK_INSTALL_PREVIOUS, &e);
    if (version && size) copy(version, size, found ? e.version : "");
    return found;
}

esp_err_t yk_install_cache_manifest_sha(const char *slug, const char *sha256, yk_install_manifest_t *out) {
    lock();
    cache_load();
    const int i = slug && sha256 && s_cache_part ? yk_cache_find(&s_cache, slug, sha256) : -1;
    if (i >= 0) {
        const yk_cache_entry_t *e = &s_cache.e[i];
        memset(out, 0, sizeof(*out));
        copy(out->slug, sizeof(out->slug), e->slug);
        copy(out->name, sizeof(out->name), e->name);
        copy(out->version, sizeof(out->version), e->version);
        copy(out->sha256, sizeof(out->sha256), e->sha256);
        copy(out->signature, sizeof(out->signature), e->signature);
        out->size = e->size;
    }
    unlock();
    return i >= 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// ---------------------------------------------------------------- USB

/**
 * install <slot> <size> <sha256> [slug]
 * Prints YK_INSTALL_READY, then reads the image as raw bytes in 4 KB chunks, answering each with YK_ACK <bytes>. Ends
 * with YK_INSTALL OK|<code>. The launcher's protocol (launcher/tools/install_usb.py), for bench tools and the desktop
 * app. The console's USB receive ring needs room for a chunk (8 KB).
 */
static int usb_install_cmd(int argc, char **argv) {
    uint8_t sha[32];
    const int slot = argc > 1 ? atoi(argv[1]) : -1;
    const uint32_t size = argc > 2 ? strtoul(argv[2], NULL, 10) : 0;
    const char *slug = argc > 4 ? argv[4] : "";
    if (argc < 4 || slot < 0 || slot >= YK_SLOT_COUNT || !size || !yk_inst_hex32(argv[3], sha)) {
        printf("YK_INSTALL BAD_REQUEST\n");
        return 1;
    }
    // Refused before anything is erased: 位置 1 needs the request to name Yay (its old image would be gone before the
    // new one's project name could be read); the other slots refuse Yay.
    const char *rule = slot == YK_INST_HOME_SLOT && !*slug ? "HOME_SLOT_RESERVED" : yk_inst_slot_rule(slot, slug);
    if (rule) {
        printf("YK_INSTALL %s slot=%d slug=%s\n", rule, slot, slug);
        return 1;
    }
    if (!claim(slot, slug, NULL, size, false, NULL, NULL)) {
        printf("YK_INSTALL BUSY\n");
        return 1;
    }
    slot_note_clear(slot);  // whatever comes over USB, it is not what this component noted
    yk_writer_t w;
    const int64_t started = esp_timer_get_time();
    uint8_t *head = malloc(CHUNK), *scratch = internal_buffer(), *buf = internal_buffer();
    const char *error = head && scratch && buf ? slot_begin(&w, slot, size, head, scratch) : "NO_MEMORY";
    const bool begun = head && scratch && buf;
    if (!error) {
        // Drop any line-ending bytes left over from the command line before the image starts.
        while (usb_serial_jtag_read_bytes(buf, CHUNK, pdMS_TO_TICKS(20)) > 0) {}
        printf("YK_INSTALL_READY chunk=%d\n", CHUNK);
        fflush(stdout);
        while (!error && w.written < size) {
            const uint32_t want = size - w.written < CHUNK ? size - w.written : CHUNK;
            uint32_t got = 0;
            while (got < want) {
                const int n = usb_serial_jtag_read_bytes(buf + got, want - got, pdMS_TO_TICKS(5000));
                if (n <= 0) break;
                got += n;
            }
            if (got < want) {
                error = "USB_TIMEOUT";
                break;
            }
            error = yk_writer_write(&w, buf, got);
            set_done(w.written);
            printf("YK_ACK %u\n", (unsigned)w.written);
            fflush(stdout);
        }
    }
    if (!error) {
        set_phase(YK_INSTALL_VERIFYING, NULL);
        error = slot_finish(&w, slot, sha, slug);
    } else if (buf) {
        // Image bytes still in flight must not reach the console as commands.
        while (usb_serial_jtag_read_bytes(buf, CHUNK, pdMS_TO_TICKS(300)) > 0) {}
    }
    if (begun) yk_writer_end(&w);
    free(head);
    free(scratch);
    free(buf);
    printf("YK_INSTALL %s slot=%d bytes=%u ms=%d from=usb\n", error ? error : "OK", slot, (unsigned)size,
           (int)((esp_timer_get_time() - started) / 1000));
    release(error);
    return error ? 1 : 0;
}

// ---------------------------------------------------------------- pkg

static void print_json(const char *prefix, cJSON *obj) {
    char *text = cJSON_PrintUnformatted(obj);
    printf("%s %s\n", prefix, text ? text : "{}");
    cJSON_free(text);
    cJSON_Delete(obj);
}

#if CONFIG_YAY_DEV
static bool s_open_after;

static void console_listener(const yk_install_status_t *s, void *ctx) {
    (void)ctx;
    static yk_install_phase_t last = YK_INSTALL_IDLE;
    if (s->phase != last || s->phase == YK_INSTALL_DONE || s->phase == YK_INSTALL_FAILED)
        printf("YK_INSTALL_PHASE %s slot=%d done=%u total=%u%s%s\n", PHASES[s->phase], s->slot, (unsigned)s->done,
               (unsigned)s->total, s->error ? " error=" : "", s->error ? s->error : "");
    last = s->phase;
    if (s->phase == YK_INSTALL_DONE && s_open_after) {
        s_open_after = false;
        yk_app_open(s->slot, s->slug, "", "{}");
    }
}

/** [slot|auto] [open] after the manifest. */
static int start_from_console(const yk_install_manifest_t *m, int argc, char **argv, int at) {
    int slot = -1;
    s_open_after = false;
    for (int i = at; i < argc; i++) {
        if (!strcmp(argv[i], "open")) s_open_after = true;
        else if (strcmp(argv[i], "auto")) slot = atoi(argv[i]);
    }
    yk_install_plan_t plan;
    yk_install_plan(m, NULL, &plan);
    printf("YK_INSTALL_PLAN slot=%d update=%d replaces=%s cached=%d%s%s\n", plan.slot, plan.update,
           plan.replaces ? plan.replace : "-", plan.cached, plan.error ? " error=" : "", plan.error ? plan.error : "");
    if (slot < 0) slot = plan.slot;
    if (slot < 0) return 1;
    return yk_install_start(m, slot, console_listener, NULL) == ESP_OK ? 0 : 1;
}
#endif

static int pkg_cmd(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "status";
    if (!strcmp(sub, "status")) {
        cJSON *obj = cJSON_CreateObject();
        yk_install_status_json(obj);
        print_json("YK_PKG", obj);
        obj = cJSON_CreateObject();
        yk_install_cache_json(obj);
        print_json("YK_PKG_CACHE", obj);
        return 0;
    }
    if (!strcmp(sub, "cache")) {
        cJSON *obj = cJSON_CreateObject();
        yk_install_cache_json(obj);
        print_json("YK_PKG_CACHE", obj);
        return 0;
    }
#if CONFIG_YAY_DEV
    if (!strcmp(sub, "install") && argc >= 8) {
        // pkg install <slug> <version> <size> <sha256> <url|-> <signature> [slot|auto] [open]: positional, so that
        // nothing needs quoting on the console.
        yk_install_manifest_t m = {.size = strtoul(argv[4], NULL, 10)};
        copy(m.slug, sizeof(m.slug), argv[2]);
        copy(m.version, sizeof(m.version), argv[3]);
        copy(m.sha256, sizeof(m.sha256), argv[5]);
        copy(m.url, sizeof(m.url), strcmp(argv[6], "-") ? argv[6] : "");
        copy(m.signature, sizeof(m.signature), argv[7]);
        return start_from_console(&m, argc, argv, 8);
    }
    if (!strcmp(sub, "cached") && argc >= 3) {
        yk_install_manifest_t m;
        if (yk_install_cache_manifest(argv[2], &m) != ESP_OK) {
            printf("YK_INSTALL NOT_CACHED slug=%s\n", argv[2]);
            return 1;
        }
        return start_from_console(&m, argc, argv, 3);
    }
    if (!strcmp(sub, "cache-slot") && argc >= 3) {
        // pkg cache-slot <slot> <signature>: the image in a slot into the cache as an installable image of its app
        // (the recovery's update on the bench, without the network: a USB install, then this, then another one).
        uint32_t len = 0;
        esp_app_desc_t desc;
        const int slot = atoi(argv[2]);
        const char *error = slot_image(slot, &len, &desc);
        char slug[32] = "";
        if (!error) {
            const char *name = strncmp(desc.project_name, "yaykit_", 7) ? desc.project_name : desc.project_name + 7;
            snprintf(slug, sizeof(slug), "%.31s", name);
            for (char *c = slug; *c; c++)
                if (*c == '_') *c = '-';
            error = slot_to_cache(slot, slug, argc > 3 ? argv[3] : "-");
        }
        return error ? 1 : 0;
    }
    if (!strcmp(sub, "cancel")) return yk_install_cancel() == ESP_OK ? 0 : 1;
    if (!strcmp(sub, "fail") && argc >= 3) {
        s_fail_downloads = atoi(argv[2]);
        printf("YK_INSTALL_FAIL next=%d\n", s_fail_downloads);
        return 0;
    }
    if (!strcmp(sub, "forget")) {
        const char *slug;
        const yk_forget_t what = yk_inst_forget_args(argc, argv, &slug);
        if (what == YK_FORGET_USAGE) {
            printf("usage: pkg forget <slug> | --all\n");
            return 1;
        }
        yk_install_cache_forget(what == YK_FORGET_ALL ? NULL : slug);
        return 0;
    }
#endif
    printf("usage: pkg [status|cache]"
#if CONFIG_YAY_DEV
           " | install <slug> <version> <size> <sha256> <url|-> <signature> [slot|auto] [open] | cached <slug> [slot]"
           " [open] | cache-slot <slot> <signature> | cancel | forget <slug>|--all | fail <n>"
#endif
           "\n");
    return 1;
}

esp_err_t yk_install_register_console(void) {
    ensure_lock();
    const esp_console_cmd_t usb = {
        .command = "install",
        .help = "install <slot> <size> <sha256> [slug]: write an app image sent over USB",
        .func = usb_install_cmd,
    };
    const esp_console_cmd_t pkg = {
        .command = "pkg",
        .help = "pkg [status|cache]: installing and the package cache",
        .func = pkg_cmd,
    };
    esp_err_t err = esp_console_cmd_register(&usb);
    return err == ESP_OK ? esp_console_cmd_register(&pkg) : err;
}

#include "yaykit_app.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/wdt_hal.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "soc/rtc.h"
#include "yaykit_key.h"
#include "yaykit_link.h"
#include "yaykit_slots.h"
#include "yk_mem.h"
#include "yk_app_logic.h"
#include "yk_app_ring.h"
#include "yk_hint.h"

#define NS "yk_sys"
#define RELEASE_WAIT_MS 3000  // GPIO0 is the download pin: never restart while it is held

static const char *TAG = "yk_app";
static yk_app_config_t s_cfg;
static char s_banner[48];
static volatile bool s_quiet;  // the banner stops repeating after the first input
static bool s_started, s_ready, s_guard_clear;

static int64_t unix_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/** 0..2 for an app slot, -1 for factory or another layout. */
static int running_slot(void) {
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (!p || p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN ||
        p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN + YK_SLOT_COUNT)
        return -1;
    return p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
}

// ---------------------------------------------------------------- yk_sys records

static esp_err_t sys_get_str(const char *key, char *out, size_t size) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = size;
    err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err;
}

static esp_err_t sys_write(const char *key, const char *str, const int64_t *i64, const uint8_t *u8) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (str) err = nvs_set_str(h, key, str);
    else if (i64) err = nvs_set_i64(h, key, *i64);
    else if (u8) err = nvs_set_u8(h, key, *u8);
    else if ((err = nvs_erase_key(h, key)) == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void sys_erase(const char *key) { sys_write(key, NULL, NULL, NULL); }

static bool sys_get_u8(const char *key, uint8_t *out) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    const bool ok = nvs_get_u8(h, key, out) == ESP_OK;
    nvs_close(h);
    return ok;
}

esp_err_t yk_app_take_crashed(char *slug, size_t size) {
    esp_err_t err = sys_get_str("crashed", slug, size);
    if (err == ESP_OK || err == ESP_ERR_NVS_INVALID_LENGTH) sys_erase("crashed");
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

// ---------------------------------------------------------------- the main firmware's update

esp_err_t yk_app_update_get(yk_update_t *out) {
    char rec[160];
    const esp_err_t err = sys_get_str("update", rec, sizeof(rec));
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err == ESP_OK && yk_update_parse(rec, out)) return ESP_OK;
    ESP_LOGW(TAG, "update record not understood; dropped");
    sys_erase("update");
    return ESP_ERR_NOT_FOUND;
}

esp_err_t yk_app_update_put(const yk_update_t *u) {
    char rec[160];
    if (yk_update_format(rec, sizeof(rec), u) < 0) return ESP_ERR_INVALID_ARG;
    return sys_write("update", rec, NULL, NULL);
}

esp_err_t yk_app_update_request_ex(const char *version, const char *sha256, const char *from, bool automatic) {
    yk_update_t u = {.state = YK_UPDATE_PENDING};
    snprintf(u.version, sizeof(u.version), "%s", version ? version : "");
    snprintf(u.sha256, sizeof(u.sha256), "%s", sha256 ? sha256 : "");
    snprintf(u.from, sizeof(u.from), "%s", from ? from : "");
    // The mark first: a record without it is a manual one, which a power cut in between must not make automatic.
    if (automatic) sys_write("update_auto", u.sha256, NULL, NULL);
    else sys_erase("update_auto");
    return yk_app_update_put(&u);
}

esp_err_t yk_app_update_request(const char *version, const char *sha256, const char *from) {
    return yk_app_update_request_ex(version, sha256, from, false);
}

bool yk_app_update_is_auto(const yk_update_t *u) {
    char sha[72];
    return sys_get_str("update_auto", sha, sizeof(sha)) == ESP_OK && yk_update_auto_matches(sha, u);
}

void yk_app_update_clear(void) {
    sys_erase("update");
    sys_erase("update_auto");
}

void yk_app_update_confirm(void) {
    yk_update_t u;
    if (yk_app_update_get(&u) == ESP_OK && u.state == YK_UPDATE_TRIAL) {
        printf("YAY_APP update confirmed version=%s\n", u.version);
        sys_erase("update");
        sys_erase("update_auto");
    }
}

void yk_app_update_fail(const char *code) {
    sys_write("update_err", code && *code ? code : "FAILED", NULL, NULL);
    sys_erase("update");
    sys_erase("update_auto");
}

esp_err_t yk_app_update_take_error(char *code, size_t size) {
    const esp_err_t err = sys_get_str("update_err", code, size);
    if (err == ESP_OK || err == ESP_ERR_NVS_INVALID_LENGTH) sys_erase("update_err");
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

esp_err_t yk_app_update_peek_error(char *code, size_t size) {
    const esp_err_t err = sys_get_str("update_err", code, size);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

static void update_stable(void *arg) {
    (void)arg;
    yk_app_update_confirm();
}

esp_err_t yk_app_update_reject(const char *version) { return sys_write("update_bad", version, NULL, NULL); }

void yk_app_update_unreject(void) { sys_erase("update_bad"); }

bool yk_app_update_rejected(const char *version) {
    char bad[32];
    return version && sys_get_str("update_bad", bad, sizeof(bad)) == ESP_OK && !strcmp(bad, version);
}

int64_t yk_app_last_used(int slot) {
    char key[12];
    snprintf(key, sizeof(key), "used_%d", slot);
    nvs_handle_t h;
    int64_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i64(h, key, &v);
        nvs_close(h);
    }
    return v;
}

// ---------------------------------------------------------------- banner and key

// yaykit_ui's `snap` streams the frame over the console; a banner in the middle would spoil it. Weak, so
// firmware without yaykit_ui still links.
bool yk_ui_capturing(void) __attribute__((weak));

static void banner_task(void *arg) {
    (void)arg;
    while (!s_quiet) {
        if (!(yk_ui_capturing && yk_ui_capturing())) printf("%s version=%s\n", s_banner, s_cfg.version);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    vTaskDelete(NULL);
}

void yk_app_input(void) { s_quiet = true; }

static void on_key(const yk_key_event_t *ev, void *ctx) {
    (void)ctx;
    s_quiet = true;
    yk_app_ring_key(ev);  // the app ring (P23), the app's own key (yk_app_on_key) and 「回到 Yay 说？」
}

esp_err_t yk_app_on_key(yk_app_key_fn fn, void *ctx) {
    if (!s_started) return ESP_ERR_INVALID_STATE;
    yk_app_ring_on_key(fn, ctx);
    return ESP_OK;
}

// ---------------------------------------------------------------- crash loop guard

/** What the last reset says about the last run, and its name for the log (NULL for an ordinary one). */
static yk_reset_t last_reset(const char **why) {
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC: *why = "panic"; return YK_RESET_CRASH;
    case ESP_RST_INT_WDT: *why = "int_wdt"; return YK_RESET_CRASH;
    case ESP_RST_TASK_WDT: *why = "task_wdt"; return YK_RESET_CRASH;
    case ESP_RST_WDT: *why = "wdt"; return YK_RESET_CRASH;
    // The supply dipped: a battery nearly empty (the board has no battery reading yet to tell more). Not counted.
    case ESP_RST_BROWNOUT: *why = "brownout"; return YK_RESET_POWER;
    default: *why = NULL; return YK_RESET_NORMAL;
    }
}

static void guard_stable(void *arg) {
    (void)arg;
    const uint8_t zero = 0;
    sys_write("boot_fail", NULL, NULL, &zero);
    printf("YAY_APP boot_ok slug=%s\n", s_cfg.slug);
}

/** The device id, kept in yk_sys for the apps that do not start yaykit_link: their record outbox (yk_app_record) and
 *  their own `yk info`. Written by any firmware that knows it (yaykit_link started before yk_app_ready()), once:
 *  only when it differs from what is kept. */
static void keep_devid(void) {
    const char *dev = yk_link_device_id();
    char stored[16] = "";
    if (dev && *dev && (sys_get_str("devid", stored, sizeof(stored)) != ESP_OK || strcmp(stored, dev))) {
        sys_write("devid", dev, NULL, NULL);
        printf("YAY_APP devid kept\n");
    }
}

/** Before anything else that could crash. Returns only when this firmware should start. */
static void guard_boot(void) {
    const char *why = NULL;
    const yk_reset_t reset = last_reset(&why);
    uint8_t stored = 0, count = 0;
    sys_get_u8("boot_fail", &stored);
    const yk_guard_action_t act = yk_boot_guard(stored, reset, s_cfg.is_home, &count);
    if (reset == YK_RESET_CRASH) printf("YAY_APP boot_fail slug=%s count=%u reset=%s\n", s_cfg.slug, count, why);
    if (reset == YK_RESET_POWER) printf("YAY_APP boot_power slug=%s count=%u reset=%s (not counted)\n", s_cfg.slug, count, why);
    if (act == YK_GUARD_RUN) {
        if (count != stored) sys_write("boot_fail", NULL, NULL, &count);  // nothing written on a normal boot
        s_guard_clear = count != 0;
#if CONFIG_YAY_DEV
        uint8_t dev = 0;
        if (sys_get_u8("dev_crash", &dev) && dev) {
            dev--;
            if (dev) sys_write("dev_crash", NULL, NULL, &dev);
            else sys_erase("dev_crash");
            printf("YAY_APP dev crash at start (%u more)\n", dev);
            abort();
        }
#endif
        return;
    }
    const uint8_t zero = 0;
    sys_write("boot_fail", NULL, NULL, &zero);
    sys_write("crashed", s_cfg.slug, NULL, NULL);
    sys_erase("dev_crash");
    printf("YAY_APP crashloop slug=%s count=%u to=%s\n", s_cfg.slug, count,
           act == YK_GUARD_HOME ? "yay" : "recovery");
    if (act == YK_GUARD_HOME) yk_return_home();
    else yk_app_recovery();
    ESP_LOGE(TAG, "could not leave; starting anyway");  // nowhere to go
}

esp_err_t yk_app_start(const yk_app_config_t *cfg) {
    if (s_started) return ESP_OK;
    if (!cfg || !cfg->slug || !cfg->version) return ESP_ERR_INVALID_ARG;
    s_cfg = *cfg;
    if (cfg->banner) snprintf(s_banner, sizeof(s_banner), "%s", cfg->banner);
    else yk_banner_name(s_banner, sizeof(s_banner), cfg->slug);
    s_started = true;
    const int slot = running_slot();
    if (slot >= 0) guard_boot();  // an app built into factory has nowhere to go back to
    if (slot >= 0) {
        const int64_t now = unix_ms();
        if (now / 1000 >= YK_APP_TIME_VALID_S) {
            char key[12];
            snprintf(key, sizeof(key), "used_%d", slot);
            sys_write(key, NULL, &now, NULL);
        }
    }
    // The hint arrow's counts, and which app this is (P24: an extension's first time shows APPS).
    yk_hint_init(s_cfg.slug);
    // The app ring in an extension; in Yay (its own) and the recovery (nothing to go to) the key is only passed on.
    yk_app_ring_start(s_cfg.slug, s_cfg.talks, s_cfg.own_double, !s_cfg.is_home && slot >= 0);
    esp_err_t err = yk_key_subscribe(on_key, NULL);
    if (err != ESP_OK) ESP_LOGW(TAG, "side key not started: no app ring");
    ESP_LOGI(TAG, "%s %s (api %d, slot %d)", s_cfg.slug, s_cfg.version, YK_API_LEVEL, slot + 1);
    return ESP_OK;
}

esp_err_t yk_app_ready(void) {
    if (s_ready) return ESP_OK;
    s_ready = true;
    yk_mem_ready();  // the 40 KB line is read from here on (yk_mem.h)
    esp_err_t err = yk_app_confirm();
    if (err != ESP_OK) ESP_LOGE(TAG, "confirm failed: %s", esp_err_to_name(err));
    keep_devid();
    if (s_guard_clear) {
        // A start that crashed before: forgiven once this one has run for a while.
        static esp_timer_handle_t timer;
        const esp_timer_create_args_t args = {.callback = guard_stable, .name = "yk_guard"};
        if (esp_timer_create(&args, &timer) == ESP_OK) esp_timer_start_once(timer, YK_CRASHLOOP_STABLE_MS * 1000LL);
    }
    yk_update_t u;
    const bool has = s_cfg.is_home && yk_app_update_get(&u) == ESP_OK;
    const int32_t confirm_ms = yk_update_confirm_after(has ? &u : NULL, s_cfg.is_home && running_slot() == 0);
    if (confirm_ms >= 0) {
        // A new Yay on trial: kept once it has run this long, online or not (hello.ok confirms sooner).
        static esp_timer_handle_t timer;
        const esp_timer_create_args_t args = {.callback = update_stable, .name = "yk_update"};
        if (esp_timer_create(&args, &timer) == ESP_OK) esp_timer_start_once(timer, confirm_ms * 1000LL);
        printf("YAY_APP update trial version=%s confirm_in=%ds\n", u.version, (int)(confirm_ms / 1000));
    }
    if (s_started && !s_cfg.quiet) xTaskCreate(banner_task, "yk_banner", 3072, NULL, 1, NULL);
    return err;
}

// ---------------------------------------------------------------- launch and result

static void copy_str(char *out, size_t size, const cJSON *item) {
    snprintf(out, size, "%s", cJSON_IsString(item) ? item->valuestring : "");
}

esp_err_t yk_app_launch(yk_launch_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    snprintf(out->params, sizeof(out->params), "{}");
    char *json = malloc(YK_APP_RECORD_MAX + 1);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t err = sys_get_str("launch", json, YK_APP_RECORD_MAX + 1);
    if (err == ESP_OK) sys_erase("launch");  // read once, whoever it was for
    cJSON *doc = err == ESP_OK ? cJSON_Parse(json) : NULL;
    free(json);
    if (!doc) return err == ESP_ERR_NVS_NOT_FOUND || err == ESP_OK ? ESP_OK : err;
    const cJSON *slug = cJSON_GetObjectItem(doc, "slug"), *at = cJSON_GetObjectItem(doc, "at");
    out->at = cJSON_IsNumber(at) ? (int64_t)at->valuedouble : 0;
    if (s_started && yk_launch_accept(cJSON_IsString(slug) ? slug->valuestring : NULL, out->at, s_cfg.slug,
                                      unix_ms() / 1000)) {
        out->present = true;
        copy_str(out->from, sizeof(out->from), cJSON_GetObjectItem(doc, "from"));
        copy_str(out->intent, sizeof(out->intent), cJSON_GetObjectItem(doc, "intent"));
        copy_str(out->nonce, sizeof(out->nonce), cJSON_GetObjectItem(doc, "nonce"));
        const cJSON *params = cJSON_GetObjectItem(doc, "params");
        if (cJSON_IsObject(params)) cJSON_PrintPreallocated((cJSON *)params, out->params, sizeof(out->params), 0);
        printf("YAY_APP launch slug=%s intent=%s from=%s\n", s_cfg.slug, out->intent, out->from);
    }
    cJSON_Delete(doc);
    return ESP_OK;
}

esp_err_t yk_app_set_result(const char *summary, const char *data_json) {
    if (!s_started) return ESP_ERR_INVALID_STATE;
    char *json = malloc(YK_APP_RECORD_MAX + 1);
    if (!json) return ESP_ERR_NO_MEM;
    const int n = yk_result_format(json, YK_APP_RECORD_MAX + 1, s_cfg.slug, summary, data_json, unix_ms() / 1000);
    esp_err_t err = n < 0 ? ESP_ERR_INVALID_SIZE : sys_write("result", json, NULL, NULL);
    free(json);
    return err;
}

esp_err_t yk_app_set_result_summary(const char *sentence, const yk_summary_t *summary, const cJSON *data) {
    char text[512];
    if (summary && yk_summary_format(text, sizeof(text), summary) < 0) return ESP_ERR_INVALID_SIZE;
    cJSON *doc = data ? cJSON_Duplicate(data, true) : cJSON_CreateObject();
    cJSON *s = summary ? cJSON_Parse(text) : NULL;
    if (!doc || !cJSON_IsObject(doc) || (summary && !s)) {
        cJSON_Delete(doc);
        cJSON_Delete(s);
        return ESP_ERR_INVALID_ARG;
    }
    if (s) {
        cJSON_DeleteItemFromObjectCaseSensitive(doc, "summary");
        cJSON_AddItemToObject(doc, "summary", s);
    }
    char *json = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (!json) return ESP_ERR_NO_MEM;
    const esp_err_t err = yk_app_set_result(sentence, json);
    cJSON_free(json);
    return err;
}

// ---------------------------------------------------------------- leaving

static struct {
    yk_app_leave_fn fn;
    void *ctx;
    uint32_t budget_ms;  // YK_APP_LEAVE_MS, or what yk_app_on_leave_ms asked for
    SemaphoreHandle_t done;
    bool left;  // it ran (or was cut): only once
} s_leave;

void yk_app_on_leave_ms(yk_app_leave_fn fn, void *ctx, uint32_t budget_ms) {
    if (s_leave.left) return;  // a wrap-up still running reads these
    s_leave.fn = fn;
    s_leave.ctx = ctx;
    s_leave.budget_ms = budget_ms < YK_APP_LEAVE_MS ? YK_APP_LEAVE_MS
                        : budget_ms > YK_APP_LEAVE_MAX_MS ? YK_APP_LEAVE_MAX_MS
                                                          : budget_ms;
}

void yk_app_on_leave(yk_app_leave_fn fn, void *ctx) { yk_app_on_leave_ms(fn, ctx, YK_APP_LEAVE_MS); }

static void leave_task(void *arg) {
    (void)arg;
    s_leave.fn(s_leave.ctx);
    xSemaphoreGive(s_leave.done);
    vTaskDelete(NULL);
}

/** The app's wrap-up (yk_app_on_leave) before a restart into other firmware: YK_APP_LEAVE_MS at most (or its
 *  yk_app_on_leave_ms), once. */
static void run_leave(const char *to) {
    if (!s_leave.fn || s_leave.left || (!s_leave.done && !(s_leave.done = xSemaphoreCreateBinary()))) return;
    s_leave.left = true;
    const int64_t t0 = esp_timer_get_time();
    // Its own task (internal stack: it may write NVS), so a wrap-up that takes too long is simply left behind.
    // 6 KB: the app's own code (a result with its summary, a record, its logs); a stack overflow here writes into
    // the heap next to it and shows later as a crash in malloc (Party Play's app.exit, 10-21, was the 3 KB caller's).
    const bool ran = xTaskCreate(leave_task, "yk_leave_cb", 6144, NULL, 5, NULL) == pdPASS &&
                     xSemaphoreTake(s_leave.done, pdMS_TO_TICKS(s_leave.budget_ms)) == pdTRUE;
    printf("YAY_APP leave to=%s ms=%lld%s\n", to, (long long)((esp_timer_get_time() - t0) / 1000),
           ran ? "" : " cut=1");
}

// ---------------------------------------------------------------- the record outbox

/** Outbox entries are "<seq>|<json>", so a slot reused by a newer record is never taken for an older one. */
static bool outbox_get(nvs_handle_t h, uint32_t seq, char *buf, size_t size, const char **json) {
    char key[8];
    yk_record_key(seq, key);
    size_t len = size;
    if (nvs_get_str(h, key, buf, &len) != ESP_OK) return false;
    char *bar = strchr(buf, '|');
    if (!bar || strtoul(buf, NULL, 10) != seq) return false;
    *json = bar + 1;
    return true;
}

/** The next sequence number. A fresh outbox (new device, NVS erased) starts at a random number, so the records
 *  after it never take the ids of records the platform already has (it keeps the first of each id). */
static uint32_t outbox_next(nvs_handle_t h) {
    uint32_t next;
    if (nvs_get_u32(h, "rseq", &next) != ESP_OK) next = esp_random() >> 1;
    return next;
}

esp_err_t yk_app_record(const char *kind, const char *data_json) {
    if (!s_started) return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    const uint32_t seq = outbox_next(h);
    char dev[16];
    snprintf(dev, sizeof(dev), "%s", yk_link_device_id());
    if (!*dev && sys_get_str("devid", dev, sizeof(dev)) != ESP_OK) strcpy(dev, "yk");
    char *buf = malloc(YK_RECORD_DATA_MAX + 320);
    const int n = buf ? snprintf(buf, 16, "%u|", (unsigned)seq) : 0;
    const int len = buf ? yk_record_format(buf + n, YK_RECORD_DATA_MAX + 320 - n, s_cfg.slug, dev, seq, kind,
                                           data_json, unix_ms())
                        : -1;
    char key[8];
    yk_record_key(seq, key);
    if (!buf) err = ESP_ERR_NO_MEM;
    else if (len < 0) err = ESP_ERR_INVALID_ARG;
    else if ((err = nvs_set_str(h, key, buf)) == ESP_OK && (err = nvs_set_u32(h, "rseq", seq + 1)) == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    free(buf);
    if (err == ESP_OK) printf("YAY_APP record seq=%u kind=%s\n", (unsigned)seq, kind);
    return err;
}

int yk_app_records_each(yk_app_record_fn fn, void *ctx) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    const uint32_t next = outbox_next(h);
    char *buf = malloc(YK_RECORD_DATA_MAX + 320);
    int got = 0;
    for (uint32_t seq = next - YK_RECORD_SLOTS; buf && seq != next; seq++) {
        const char *json;
        if (!outbox_get(h, seq, buf, YK_RECORD_DATA_MAX + 320, &json)) continue;
        got++;
        if (!fn(seq, json, ctx)) break;
    }
    free(buf);
    nvs_close(h);
    return got;
}

esp_err_t yk_app_records_drop(uint32_t seq) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    const uint32_t next = outbox_next(h);
    char *buf = malloc(YK_RECORD_DATA_MAX + 320);
    for (uint32_t s = next - YK_RECORD_SLOTS; buf && s != next && seq - s < YK_RECORD_SLOTS; s++) {
        const char *json;
        char key[8];
        yk_record_key(s, key);
        if (outbox_get(h, s, buf, YK_RECORD_DATA_MAX + 320, &json)) nvs_erase_key(h, key);
    }
    free(buf);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t yk_app_take_result(char *json, size_t size) {
    esp_err_t err = sys_get_str("result", json, size);
    if (err == ESP_OK || err == ESP_ERR_NVS_INVALID_LENGTH) sys_erase("result");
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

static void wait_release(void) {
    if (!yk_key_wait_release(RELEASE_WAIT_MS)) ESP_LOGW(TAG, "side key still held; restarting anyway");
}

// Leaving always ends in a restart. Should anything on the way stall for good (the app's wrap-up, a lock another task
// holds, the flash), the RTC watchdog resets the whole chip after LEAVE_GUARD_MS: the task watchdog only watches the
// idle tasks and would let a blocked task wait forever. The bootloader sets the RTC watchdog up anew on every boot.
#define LEAVE_GUARD_MS (YK_APP_LEAVE_MAX_MS + RELEASE_WAIT_MS + 4000)

static void leave_guard(bool on) {
    wdt_hal_context_t rtc = RWDT_HAL_CONTEXT_DEFAULT();
    if (on) {
        const uint32_t ticks = (uint32_t)((uint64_t)LEAVE_GUARD_MS * rtc_clk_slow_freq_get_hz() / 1000);
        wdt_hal_init(&rtc, WDT_RWDT, 0, false);
        wdt_hal_write_protect_disable(&rtc);
        wdt_hal_config_stage(&rtc, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_RTC);
        wdt_hal_enable(&rtc);
    } else {
        wdt_hal_write_protect_disable(&rtc);
        wdt_hal_disable(&rtc);
    }
    wdt_hal_write_protect_enable(&rtc);
}

/** The last lines out before the restart, so a log shows how far leaving got (the restart cuts what is queued). */
static void flush_log(void) {
    fflush(stdout);
    fsync(fileno(stdout));
}

/** A restart that did not happen (the slot could not be booted): the guard is off again. */
static esp_err_t left(esp_err_t err) {
    leave_guard(false);
    ESP_LOGE(TAG, "could not leave: %s", esp_err_to_name(err));
    return err;
}

// Leaving runs in a task of its own, with room for the image check and its logs: the caller may be a small task (the
// side key's 4 KB, a remote command's, an app's UI task). A 3 KB one overflowed checking Yay's image (`yk cmd
// app.exit`, 10-21). The caller waits; it gets an answer only when leaving failed.
#define LEAVE_STACK 6144

typedef struct {
    int what;  // 0 open, 1 home, 2 recovery
    int slot;
    const char *slug, *intent, *params_json;
    esp_err_t err;
    SemaphoreHandle_t done;
} leave_job_t;

static esp_err_t do_open(const leave_job_t *j);
static esp_err_t do_return_home(void);
static esp_err_t do_recovery(void);

static void leave_job_task(void *arg) {
    leave_job_t *j = arg;
    j->err = j->what == 0 ? do_open(j) : j->what == 1 ? do_return_home() : do_recovery();
    xSemaphoreGive(j->done);
    vTaskDelete(NULL);
}

static volatile bool s_leave_held;  // yk_app_hold_leave: a slot is being written

void yk_app_hold_leave(bool on) { s_leave_held = on; }

bool yk_app_leave_held(void) { return s_leave_held; }

/** Refused while a slot is being written (yk_app_hold_leave): leaving restarts, and that would cut it short. */
static bool leave_refused(void) {
    if (!s_leave_held) return false;
    printf("YAY_APP leave refused=BUSY (an app is being written)\n");
    return true;
}

static esp_err_t in_leave_task(leave_job_t *j) {
    if (leave_refused()) return ESP_ERR_INVALID_STATE;
    j->err = ESP_FAIL;
    j->done = xSemaphoreCreateBinary();
    if (!j->done || xTaskCreate(leave_job_task, "yk_leave", LEAVE_STACK, j, 5, NULL) != pdPASS) {
        if (j->done) vSemaphoreDelete(j->done);
        ESP_LOGW(TAG, "no room for the leaving task: leaving from the caller's");
        return j->what == 0 ? do_open(j) : j->what == 1 ? do_return_home() : do_recovery();
    }
    xSemaphoreTake(j->done, portMAX_DELAY);
    vSemaphoreDelete(j->done);
    return j->err;
}

esp_err_t yk_app_open(int slot, const char *slug, const char *intent, const char *params_json) {
    leave_job_t j = {.what = 0, .slot = slot, .slug = slug, .intent = intent, .params_json = params_json};
    return in_leave_task(&j);
}

esp_err_t yk_return_home(void) {
    leave_job_t j = {.what = 1};
    return in_leave_task(&j);
}

esp_err_t yk_return_home_with(const char *intent, const char *params_json) {
    if (leave_refused()) return ESP_ERR_INVALID_STATE;  // before the record: it would wait for the next start
    char nonce[17];
    snprintf(nonce, sizeof(nonce), "%08" PRIx32 "%08" PRIx32, esp_random(), esp_random());
    char *json = malloc(YK_APP_RECORD_MAX + 1);
    if (!json) return ESP_ERR_NO_MEM;
    const int n = yk_launch_format(json, YK_APP_RECORD_MAX + 1, s_started ? s_cfg.slug : YK_APP_HOME_SLUG,
                                   YK_APP_HOME_SLUG, intent, params_json, unix_ms() / 1000, nonce);
    esp_err_t err = n < 0 ? ESP_ERR_INVALID_SIZE : sys_write("launch", json, NULL, NULL);
    free(json);
    if (err != ESP_OK) return err;
    printf("YAY_APP home_with intent=%s params=%s\n", intent ? intent : "", params_json ? params_json : "{}");
    err = yk_return_home();
    sys_erase("launch");  // still here: Yay could not be started
    return err;
}

esp_err_t yk_app_recovery(void) {
    leave_job_t j = {.what = 2};
    return in_leave_task(&j);
}

static esp_err_t do_open(const leave_job_t *job) {
    const int slot = job->slot;
    const char *slug = job->slug, *intent = job->intent, *params_json = job->params_json;
    char nonce[17];
    snprintf(nonce, sizeof(nonce), "%08" PRIx32 "%08" PRIx32, esp_random(), esp_random());
    char *json = malloc(YK_APP_RECORD_MAX + 1);
    if (!json) return ESP_ERR_NO_MEM;
    const int n = yk_launch_format(json, YK_APP_RECORD_MAX + 1, s_started ? s_cfg.slug : YK_APP_HOME_SLUG, slug,
                                   intent, params_json, unix_ms() / 1000, nonce);
    esp_err_t err = n < 0 ? ESP_ERR_INVALID_SIZE : sys_write("launch", json, NULL, NULL);
    free(json);
    if (err != ESP_OK) return err;
    printf("YAY_APP open slot=%d slug=%s intent=%s\n", slot + 1, slug, intent ? intent : "");
    leave_guard(true);
    run_leave(slug);
    wait_release();
    flush_log();
    err = yk_slot_boot(slot);
    sys_erase("launch");  // still here: the slot could not be booted
    return left(err);
}

static esp_err_t do_return_home(void) {
    yk_slot_t s0;
    const bool read = yk_slot_read(0, &s0) == ESP_OK;
    const int home = read ? yk_home_slot(s0.slug, s0.state) : -1;
    printf("YAY_APP return to=%s\n", home == 0 ? "yay" : "recovery");
    leave_guard(true);
    run_leave(home == 0 ? "yay" : "recovery");
    wait_release();
    flush_log();
    return left(home == 0 ? yk_slot_boot(0) : yk_return_to_launcher());
}

static esp_err_t do_recovery(void) {
    const uint8_t one = 1;
    esp_err_t err = sys_write("recovery", NULL, NULL, &one);
    if (err != ESP_OK) return err;
    printf("YAY_APP recovery\n");
    leave_guard(true);
    run_leave("recovery");
    wait_release();
    flush_log();
    err = yk_return_to_launcher();  // back here only when it failed (a recovery that is not whole: VALIDATE_FAILED)
    // Still here: the flag goes, or the recovery's next start (a computer's install, a crash) would stay on its page
    // instead of forwarding to Yay (yk_recovery_forward), as if someone had asked for it.
    sys_erase("recovery");
    return left(err);
}

bool yk_app_home_usable(void) {
    yk_slot_t s0;
    return yk_slot_read(0, &s0) == ESP_OK && yk_home_slot(s0.slug, s0.state) == 0;
}

bool yk_app_should_forward(void) {
    uint8_t flag = 0;
    const bool asked = sys_get_u8("recovery", &flag) && flag;
    if (asked) sys_erase("recovery");  // for this boot only
    yk_slot_t s0;
    if (yk_slot_read(0, &s0) != ESP_OK) return false;
    return yk_recovery_forward(asked, s0.slug, s0.state);
}

// ---------------------------------------------------------------- commands

static void later_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(200));  // let the reply reach the host first
    if ((intptr_t)arg) yk_app_recovery();
    else yk_return_home();
    vTaskDelete(NULL);
}

esp_err_t yk_app_command(const char *name, const cJSON *args, cJSON *reply, const char **code) {
    (void)args;
    if (!strcmp(name, "app.status")) {
        cJSON_AddStringToObject(reply, "slug", s_cfg.slug ? s_cfg.slug : "");
        cJSON_AddStringToObject(reply, "version", s_cfg.version ? s_cfg.version : "");
        cJSON_AddNumberToObject(reply, "apiLevel", YK_API_LEVEL);
        cJSON_AddNumberToObject(reply, "slot", running_slot() + 1);
        cJSON_AddBoolToObject(reply, "ready", s_ready);
        return ESP_OK;
    }
    const bool recovery = !strcmp(name, "app.recovery");
    if (recovery || !strcmp(name, "app.exit")) {
        if (s_leave_held) {
            *code = "BUSY";
            return ESP_ERR_INVALID_STATE;
        }
        if (!recovery && s_cfg.is_home) {
            *code = "ALREADY_HOME";
            return ESP_ERR_INVALID_STATE;
        }
        cJSON_AddStringToObject(reply, "to", recovery ? "recovery" : "yay");
        xTaskCreate(later_task, "yk_later", 3072, (void *)(intptr_t)recovery, 5, NULL);  // leaving: its own task
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static int app_cmd(int argc, char **argv) {
    yk_app_input();
    const char *sub = argc > 1 ? argv[1] : "status";
    if ((!strcmp(sub, "exit") || !strcmp(sub, "recovery")) && leave_refused()) return 1;
    if (!strcmp(sub, "exit")) {
        if (s_cfg.is_home) {
            printf("YAY_APP already home\n");
            return 1;
        }
        return yk_return_home() == ESP_OK ? 0 : 1;
    }
    if (!strcmp(sub, "recovery")) return yk_app_recovery() == ESP_OK ? 0 : 1;
    if (!strcmp(sub, "mem")) {
        // min_since_ready: the 40 KB line (yk_mem.h); min_ever: the figure from before 10-24, while they are compared.
        printf("YAY_APP mem internal=%u min_since_ready=%u min_ever=%u largest=%u psram=%u ready=%d\n",
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)yk_mem_min_since_ready(),
               (unsigned)yk_mem_min_ever(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), s_ready);
        return 0;
    }
    printf("YAY_APP status slug=%s version=%s api=%d slot=%d ready=%d\n", s_cfg.slug ? s_cfg.slug : "",
           s_cfg.version ? s_cfg.version : "", YK_API_LEVEL, running_slot() + 1, s_ready);
    return 0;
}

// ---------------------------------------------------------------- development commands

#define DEV_MAX 30  // the main firmware registers 26
static struct {
    const char *name, *help;
    yk_app_dev_fn fn;
} s_dev[DEV_MAX];
static yk_app_say_fn s_say;

esp_err_t yk_app_dev_register(const char *name, const char *help, yk_app_dev_fn fn) {
    for (int i = 0; i < DEV_MAX; i++)
        if (!s_dev[i].name) {
            s_dev[i].name = name, s_dev[i].help = help, s_dev[i].fn = fn;
            return ESP_OK;
        }
    ESP_LOGE(TAG, "no room for `yay %s` (DEV_MAX %d)", name, DEV_MAX);
    return ESP_ERR_NO_MEM;
}

void yk_app_dev_on_say(yk_app_say_fn fn) { s_say = fn; }

#if CONFIG_YAY_DEV
static int find_slot(const char *slug) {
    for (int i = 0; i < YK_SLOT_COUNT; i++) {
        yk_slot_t s;
        if (yk_slot_read(i, &s) == ESP_OK && s.present && !strcmp(s.slug, slug)) return i;
    }
    return -1;
}
#endif

static bool print_record(uint32_t seq, const char *json, void *ctx) {
    (void)ctx;
    printf("YAY_APP record seq=%u %s\n", (unsigned)seq, json);
    return true;
}

static int yay_cmd(int argc, char **argv) {
    yk_app_input();
    const char *sub = argc > 1 ? argv[1] : "";
    if (!strcmp(sub, "tap")) return yk_key_simulate(80) == ESP_OK ? 0 : 1;
    if (!strcmp(sub, "double")) {
        yk_key_simulate(80);
        vTaskDelay(pdMS_TO_TICKS(80 + 120));
        return yk_key_simulate(80) == ESP_OK ? 0 : 1;
    }
    if (!strcmp(sub, "hold") && argc > 2) return yk_key_simulate(atoi(argv[2])) == ESP_OK ? 0 : 1;
    if (!strcmp(sub, "say") && argc > 2) {
        const int seconds = atoi(argv[2]);
        char text[256] = "";
        for (int i = 3; i < argc; i++) {
            if (i > 3) strncat(text, " ", sizeof(text) - strlen(text) - 1);
            strncat(text, argv[i], sizeof(text) - strlen(text) - 1);
        }
        if (s_say) s_say(seconds, text);
        return yk_key_simulate(seconds > 0 ? seconds * 1000 : 1000) == ESP_OK ? 0 : 1;
    }
    if (!strcmp(sub, "records")) {
        if (argc > 3 && !strcmp(argv[2], "drop")) return yk_app_records_drop(strtoul(argv[3], NULL, 10)) == ESP_OK ? 0 : 1;
        printf("YAY_APP records n=%d\n", yk_app_records_each(print_record, NULL));
        return 0;
    }
#if CONFIG_YAY_DEV
    // Development only from here: release builds keep the acceptance commands above (CONFIG_YAY_ACCEPT).
    if (!strcmp(sub, "ring") && argc > 2) {
        const int r = yk_app_ring_dev_tap(argv[2]);  // an extension's ring; in Yay its own `yay ring` (below)
        if (r >= 0) return r;
    }
    if (!strcmp(sub, "crash")) {
        const int boots = argc > 2 ? atoi(argv[2]) : 0;
        if (boots > 0) {
            const uint8_t n = boots > 9 ? 9 : (uint8_t)boots;
            sys_write("dev_crash", NULL, NULL, &n);
        }
        printf("YAY_APP dev crash now (then %d at start)\n", boots);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));
        abort();
    }
    if (!strcmp(sub, "launch") && argc > 2) {
        const int slot = find_slot(argv[2]);
        if (slot < 0) {
            printf("YAY_APP no slot holds %s\n", argv[2]);
            return 1;
        }
        return yk_app_open(slot, argv[2], "dev", argc > 3 ? argv[3] : NULL) == ESP_OK ? 0 : 1;
    }
    if (!strcmp(sub, "record") && argc > 2) return yk_app_record(argv[2], argc > 3 ? argv[3] : NULL) == ESP_OK ? 0 : 1;
    for (int i = 0; i < DEV_MAX; i++)
        if (s_dev[i].name && !strcmp(s_dev[i].name, sub)) return s_dev[i].fn(argc - 1, argv + 1);
    printf("yay tap | double | hold <ms> | say <s> [text] | records [drop <seq>] | ring <slug>|yay|paper"
           " | launch <slug> [json] | crash [boots] | record <kind> [json]");
    for (int i = 0; i < DEV_MAX; i++)
        if (s_dev[i].name) printf(" | %s", s_dev[i].help ? s_dev[i].help : s_dev[i].name);
#else
    printf("yay tap | double | hold <ms> | say <s> | records [drop <seq>]");
#endif
    printf("\n");
    return 1;
}

esp_err_t yk_app_register_console(void) {
    const esp_console_cmd_t app = {.command = "app", .help = "app [status|exit|recovery|mem]", .func = app_cmd};
    esp_err_t err = esp_console_cmd_register(&app);
#if CONFIG_YAY_DEV || CONFIG_YAY_ACCEPT
    if (err == ESP_OK) {
#if CONFIG_YAY_DEV
        const char *help = "development: simulate keys and events";
#else
        const char *help = "acceptance: simulate the side key, the record outbox";
#endif
        const esp_console_cmd_t yay = {.command = "yay", .help = help, .func = yay_cmd};
        err = esp_console_cmd_register(&yay);
    }
#else
    (void)yay_cmd;
#endif
    return err;
}

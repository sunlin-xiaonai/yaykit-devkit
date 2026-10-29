// The app ring and 「回到 Yay 说？」 in an extension (P23; device-controls.md 1.2, 1.3, 4). yaykit_app takes the
// double click and opens yaykit_ui's ring over the app: a tap on the middle goes home, on another app opens it, on an
// app not on the device goes home with {"then":"install"} (only Yay installs), on 「更多」 (the fifth place,
// app-ecosystem.md 4.5) goes home with {"then":"more"}: Yay opens 「我的应用」; on 「设置」 (the last place since 0.1.6)
// with {"then":"settings"}: Yay opens its settings. A hold the app did not use, in an app
// with no conversation of its own, asks 「回到 Yay 说？」. Any press takes the hint arrow away (P24: yk_hint, the
// app's own APPS hint included), before the app hears of it.
//
// The key task only decides (yk_keys_route, pure, host tested) and takes the key while ours (yk_key_grab). A small
// task of ours waits for the board lock and hands the drawing to the LVGL task (lv_async_call: measuring the names
// takes TinyTTF more stack than ours), so the key task never waits for a frame. Async calls may run newest first:
// each one checks that what it does is still wanted.
#include "yk_app_ring.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "yaykit_key.h"
#include "yaykit_slots.h"
#include "yk_app_logic.h"
#include "yk_apps.h"
#include "yk_catalog.h"
#include "yk_hint.h"

static const char *TAG = "yk_ring";

#define REPORT_MS 500  // the opening's numbers, once its motion (~300 ms) is done

enum { J_RING_OPEN, J_RING_CLOSE, J_ASK_OPEN, J_ASK_CLOSE, J_TAPPED, J_ASKED, J_HINT_HIDE };
typedef struct {
    uint8_t what;
    char slug[32];  // J_TAPPED, J_ASKED: what was tapped ("" nothing)
} job_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static yk_keys_t s_keys;  // under s_mux
static bool s_active, s_talks, s_own_double;
static bool s_home_on_double;  // no room for our task: the double click goes straight home, as before level 3
static const char *s_slug;  // this app's
static yk_app_key_fn s_app_fn;
static void *s_app_ctx;
static QueueHandle_t s_jobs;
static volatile bool s_leaving;  // restarting into another firmware: the key does nothing more
static int64_t s_key_ms;         // the double click that opened the ring (YAY_RING lines)
static int64_t s_key_down_ms;    // the press that took a hint away (YAY_HINT lines)
static volatile int s_ring_frame;  // 1: its first frame is due; 2: being drawn
static bool s_refr_hooked;

// What the ring shows, the task's (yk_apps_open copies the items; these stay for the tap's answer).
typedef struct {
    char slug[32], name[48];
    bool installed;
    int slot;  // 1-2: ota_1, ota_2
} ring_app_t;
static ring_app_t s_app[YK_APPS_MAX];
static yk_apps_item_t s_item[YK_APPS_MAX];
static int s_n;
static yk_catalog_t *s_cat;  // PSRAM, read at each open (Yay may have written it since)
// What 「正在打开 …」 names, for the LVGL task
static char s_leave_slug[32], s_leave_name[48];

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static bool post(uint8_t what, const char *slug) {
    job_t j = {.what = what};
    snprintf(j.slug, sizeof(j.slug), "%s", slug ? slug : "");
    if (s_jobs && xQueueSend(s_jobs, &j, 0) == pdTRUE) return true;
    ESP_LOGW(TAG, "job %u dropped", what);
    return false;
}

static void key(const yk_key_event_t *ev, void *ctx);

/** Nothing of ours up or under way: the key goes back to the app's subscribers. */
static void release_if_idle(void) {
    taskENTER_CRITICAL(&s_mux);
    const bool idle = yk_keys_idle(&s_keys);
    taskEXIT_CRITICAL(&s_mux);
    if (idle && !s_leaving) yk_key_grab(NULL, NULL);
}

static void closed_by_itself(void) {
    taskENTER_CRITICAL(&s_mux);
    yk_keys_closed(&s_keys);
    taskEXIT_CRITICAL(&s_mux);
    release_if_idle();
}

// ---------------------------------------------------------------- in the LVGL task (lock held)

static void on_ring(const char *slug, void *ctx) {
    (void)ctx;
    closed_by_itself();
    post(J_TAPPED, slug);
}

static void on_ask(const char *slug, void *ctx) {
    (void)ctx;
    closed_by_itself();
    post(J_ASKED, slug);
}

static void on_refr(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        if (s_ring_frame == 1) s_ring_frame = 2;
    } else if (s_ring_frame == 2) {
        s_ring_frame = 0;  // the ring's first frame is on the panel (its motion goes on for ~300 ms)
        printf("YAY_RING shown from=%s since_key_ms=%lld\n", s_slug, (long long)(now_ms() - s_key_ms));
    }
}

static void report(lv_timer_t *t) {
    yk_apps_stats_t st;
    yk_apps_stats(&st);
    printf("YAY_RING drawn from=%s open_ms=%d frames=%d since_key_ms=%lld internal=%u internal_min=%u\n", s_slug,
           st.open_ms, st.frames, (long long)(now_ms() - s_key_ms), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    lv_timer_delete(t);
}

static void ring_open_async(void *arg) {
    (void)arg;
    taskENTER_CRITICAL(&s_mux);
    const bool want = s_keys.ring;
    taskEXIT_CRITICAL(&s_mux);
    if (!want || s_leaving) return;  // a press closed it before it opened
    if (!s_refr_hooked) {
        lv_display_add_event_cb(lv_display_get_default(), on_refr, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(lv_display_get_default(), on_refr, LV_EVENT_REFR_READY, NULL);
        s_refr_hooked = true;
    }
    if (!yk_apps_open(lv_screen_active(), s_item, s_n, s_slug, on_ring, NULL)) {
        closed_by_itself();
        return;
    }
    s_ring_frame = 1;
    yk_apps_stats_t st;
    yk_apps_stats(&st);
    printf("YAY_RING open from=%s n=%d setup_ms=%d after_ms=%lld internal=%u internal_min=%u\n", s_slug, s_n,
           st.setup_ms, (long long)(now_ms() - s_key_ms), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    lv_timer_create(report, REPORT_MS, NULL);
}

static void ring_close_async(void *arg) {
    (void)arg;
    taskENTER_CRITICAL(&s_mux);
    const bool open = s_keys.ring;
    taskEXIT_CRITICAL(&s_mux);
    if (!open) yk_apps_close(true);  // (no callback)
}

static void ask_open_async(void *arg) {
    (void)arg;
    taskENTER_CRITICAL(&s_mux);
    const bool want = s_keys.ask;
    taskEXIT_CRITICAL(&s_mux);
    if (!want || s_leaving) return;
    if (!yk_apps_ask(lv_screen_active(), on_ask, NULL)) closed_by_itself();
    printf("YAY_APP ask_talk from=%s\n", s_slug);
}

static void ask_close_async(void *arg) {
    (void)arg;
    taskENTER_CRITICAL(&s_mux);
    const bool open = s_keys.ask;
    taskEXIT_CRITICAL(&s_mux);
    if (!open) yk_apps_ask_close();
}

static void hint_hide_async(void *arg) {
    (void)arg;
    if (!yk_hint_is_shown()) return;
    yk_hint_hide();
    printf("YAY_HINT hidden by=key from=%s since_key_ms=%lld\n", s_slug, (long long)(now_ms() - s_key_down_ms));
}

static void leaving_async(void *arg) {
    (void)arg;
    yk_apps_close(false);
    yk_apps_ask_close();
    yk_apps_leaving(s_leave_slug, s_leave_name[0] ? s_leave_name : NULL);
}

static void leaving_clear_async(void *arg) {
    (void)arg;
    yk_apps_leaving_clear();
}

// ---------------------------------------------------------------- our task

static bool ui(lv_async_cb_t fn) {
    if (board_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "no board lock");
        return false;
    }
    lv_async_call(fn, NULL);
    board_unlock();
    return true;
}

/** The ring's apps: those on the device (the most recently used first), then the catalog's not on it, and 「更多」
 *  (yk_apps_pick_ring; the package cache is Yay's: an extension does not read it). A slot whose image will not start
 *  is left out. */
static void build(void) {
    yk_slot_t slot[YK_SLOT_COUNT];
    yk_apps_slot_t inst[2];
    int n = 0;
    for (int i = 1; i < YK_SLOT_COUNT && n < 2; i++) {
        if (yk_slot_read(i, &slot[i]) != ESP_OK || !slot[i].present) continue;
        if (!strcmp(slot[i].state, "invalid") || !strcmp(slot[i].state, "aborted")) continue;
        inst[n++] = (yk_apps_slot_t){.slug = slot[i].slug, .used_ms = yk_app_last_used(i)};
    }
    if (!s_cat) s_cat = heap_caps_calloc(1, sizeof(*s_cat), MALLOC_CAP_SPIRAM);
    const bool cat = s_cat && yk_catalog_read(s_cat) == ESP_OK;
    yk_apps_item_t picked[YK_APPS_MAX];
    s_n = yk_apps_pick_ring(inst, n, NULL, 0, cat ? s_cat : NULL, picked, YK_APPS_MAX);
    for (int i = 0; i < s_n; i++) {
        ring_app_t *a = &s_app[i];
        snprintf(a->slug, sizeof(a->slug), "%s", picked[i].slug);
        a->installed = picked[i].installed;
        a->slot = 0;
        for (int k = 1; k < YK_SLOT_COUNT && a->installed; k++)
            if (slot[k].present && !strcmp(slot[k].slug, a->slug)) a->slot = k;
        // The store's name (Yay keeps it in the catalog for those not on the device), else the built-in one.
        const yk_catalog_entry_t *e = cat ? yk_catalog_find(s_cat, a->slug) : NULL;
        const bool own = !strcmp(a->slug, YK_APPS_MORE) || !strcmp(a->slug, YK_APPS_SETTINGS);  // yk_apps names them
        snprintf(a->name, sizeof(a->name), "%s", own ? "" : e && e->name[0] ? e->name : yk_app_display_name(a->slug));
        s_item[i] = (yk_apps_item_t){.slug = a->slug, .name = own ? NULL : a->name, .installed = a->installed};
    }
}

/** 「正在打开 …」 (or 「正在回到 Yay」) on black, a frame to show it, and nothing more from the key. */
static void leave(const char *slug, const char *name) {
    s_leaving = true;
    yk_key_grab(key, NULL);  // and `key` does nothing now
    snprintf(s_leave_slug, sizeof(s_leave_slug), "%s", slug);
    snprintf(s_leave_name, sizeof(s_leave_name), "%s", name ? name : "");
    ui(leaving_async);
    vTaskDelay(pdMS_TO_TICKS(120));  // a frame or two
}

static void left_not(esp_err_t err) {
    ESP_LOGE(TAG, "could not leave: %s", esp_err_to_name(err));
    ui(leaving_clear_async);  // back to the app, not a black screen
    s_leaving = false;
    release_if_idle();
}

static void tapped(const char *slug) {
    const ring_app_t *a = NULL;
    for (int i = 0; i < s_n; i++)
        if (!strcmp(s_app[i].slug, slug)) a = &s_app[i];
    // 「更多」, 「设置」 and an app not on the device go home with what Yay is to do (yk_ring_tapped: {"then":"more"},
    // {"then":"settings"}, {"then":"install",…}); only Yay installs (6.8), and 「我的应用」 and the settings are Yay's.
    char params[96];
    const yk_ring_go_t go = yk_ring_tapped(slug, s_slug, a && a->installed && a->slot > 0, params, sizeof(params));
    if (go == YK_RING_STAY || (go == YK_RING_HOME_WITH && !a && strcmp(slug, YK_APPS_MORE))) return;
    printf("YAY_RING tapped=%s from=%s installed=%d at=%lld\n", slug, s_slug, a && a->installed, (long long)now_ms());
    switch (go) {
    case YK_RING_HOME:
        leave(YK_APPS_HOME, NULL);
        left_not(yk_return_home());
        break;
    case YK_RING_OPEN:
        leave(slug, a->name);
        left_not(yk_app_open(a->slot, slug, "home", NULL));
        break;
    case YK_RING_HOME_WITH:
        leave(YK_APPS_HOME, NULL);
        left_not(yk_return_home_with("ring", params));
        break;
    default: break;
    }
}

static void asked(const char *slug) {
    if (strcmp(slug, YK_APPS_HOME)) return;  // 8 s untouched: it stays here
    printf("YAY_APP ask_talk yes from=%s\n", s_slug);
    leave(YK_APPS_HOME, NULL);
    left_not(yk_return_home_with("talk", "{\"then\":\"talk\"}"));  // Yay points at its key (P24)
}

static void task(void *arg) {
    (void)arg;
    job_t j;
    for (;;) {
        if (xQueueReceive(s_jobs, &j, portMAX_DELAY) != pdTRUE) continue;
        switch (j.what) {
        case J_RING_OPEN:
            build();
            if (!ui(ring_open_async)) closed_by_itself();  // never opened: the key goes back to the app
            break;
        case J_RING_CLOSE: ui(ring_close_async); break;
        case J_ASK_OPEN:
            if (!ui(ask_open_async)) closed_by_itself();
            break;
        case J_ASK_CLOSE: ui(ask_close_async); break;
        case J_TAPPED: tapped(j.slug); break;
        case J_ASKED: asked(j.slug); break;
        case J_HINT_HIDE: ui(hint_hide_async); break;
        }
    }
}

// ---------------------------------------------------------------- the key task

static void key(const yk_key_event_t *ev, void *ctx) {
    (void)ctx;
    if (s_leaving) return;
    // The hint goes the moment the key goes down, whatever the press turns out to be (only when one is up: a read
    // of yk_hint's own pointers, the drawing in the LVGL task).
    if (s_active && ev->type == YK_KEY_DOWN && yk_hint_is_shown()) {
        s_key_down_ms = ev->at_ms;
        post(J_HINT_HIDE, NULL);
    }
    if (!s_active) {
        if (s_app_fn) s_app_fn(ev, s_app_ctx);
        if (s_home_on_double && ev->type == YK_KEY_DOUBLE && !s_own_double) {
            printf("YAY_APP return from=%s by=double\n", s_slug);
            yk_return_home();
        }
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    const yk_keys_do_t d = yk_keys_route(&s_keys, ev->type, ev->at_ms, s_own_double);
    taskEXIT_CRITICAL(&s_mux);
    switch (d) {
    case YK_KEYS_APP:
        if (s_app_fn) {
            const bool used = s_app_fn(ev, s_app_ctx);
            if (ev->type == YK_KEY_HOLD) {
                taskENTER_CRITICAL(&s_mux);
                const bool ask = yk_keys_ask(&s_keys, used, s_talks);
                taskEXIT_CRITICAL(&s_mux);
                if (ask && post(J_ASK_OPEN, NULL)) yk_key_grab(key, NULL);
                else if (ask) closed_by_itself();  // no room for the job: no card, the key stays the app's
            }
        }
        break;
    case YK_KEYS_RING_OPEN:
        if (s_app_fn) s_app_fn(ev, s_app_ctx);  // the app learns of it, as before (a tap it held back is off)
        s_key_ms = ev->at_ms;
        printf("YAY_APP ring from=%s by=double\n", s_slug);
        if (post(J_RING_OPEN, NULL)) yk_key_grab(key, NULL);
        else closed_by_itself();  // no room for the job: no ring, the key stays the app's
        break;
    case YK_KEYS_RING_CLOSE: post(J_RING_CLOSE, NULL); break;
    case YK_KEYS_ASK_CLOSE: post(J_ASK_CLOSE, NULL); break;
    case YK_KEYS_EAT: break;
    }
    release_if_idle();
}

void yk_app_ring_key(const yk_key_event_t *ev) { key(ev, NULL); }

int yk_app_ring_dev_tap(const char *what) {
    if (!s_active) return -1;
    if (board_lock(500) != ESP_OK) return 1;
    const bool open = yk_apps_tap(strcmp(what, "paper") ? what : NULL);
    board_unlock();
    if (!open) printf("YAY_RING not open\n");
    return open ? 0 : 1;
}

void yk_app_ring_on_key(yk_app_key_fn fn, void *ctx) {
    s_app_ctx = ctx;
    s_app_fn = fn;
}

void yk_app_ring_start(const char *slug, bool talks, bool own_double, bool active) {
    s_slug = slug;
    s_talks = talks;
    s_own_double = own_double;
    yk_keys_init(&s_keys);
    if (!active) return;
    s_jobs = xQueueCreate(6, sizeof(job_t));  // a hint to take away as well as the ring or the card
    // 3 KB: the board lock, the slots and the catalog (into PSRAM), and leaving (its own task does the work).
    if (!s_jobs || xTaskCreate(task, "yk_ring", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no room: the double click goes straight home");
        s_home_on_double = true;
        return;
    }
    s_active = true;
}

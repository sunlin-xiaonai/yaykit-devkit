#include "yaykit_key.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "yk_key_sm.h"

#define KEY_GPIO     GPIO_NUM_0
#define SETTLE_MS    25  // contact bounce: after acting on an edge, look again once it has settled
#define MAX_SUBS     4
#define NOTE_EDGE    1u
#define NOTE_SIM     2u
#define TASK_PRIO    12  // above LVGL (6) and audio (7, 8): key timing must not depend on the renderer

static const char *TAG = "yk_key";
static TaskHandle_t s_task;
static yk_key_sm_t s_sm;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    yk_key_cb_t cb;
    void *ctx;
} s_subs[MAX_SUBS], s_grab;  // s_grab.cb set: every event goes to it alone
static volatile int64_t s_edge_us;     // first edge since the task last looked, 0 = none
static QueueHandle_t s_sim;            // simulated presses waiting to start (hold ms each)
static int64_t s_sim_release_ms = -1;  // when the simulated press in progress ends

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static bool level_pressed(void) { return gpio_get_level(KEY_GPIO) == 0; }

const char *yk_key_event_name(yk_key_event_type_t type) {
    switch (type) {
    case YK_KEY_DOWN: return "down";
    case YK_KEY_HOLD: return "hold";
    case YK_KEY_UP: return "up";
    case YK_KEY_TAP: return "tap";
    case YK_KEY_DOUBLE: return "double";
    }
    return "?";
}

static void IRAM_ATTR on_edge(void *arg) {
    (void)arg;
    if (!s_edge_us) s_edge_us = esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    xTaskNotifyFromISR(s_task, NOTE_EDGE, eSetBits, &woken);
    portYIELD_FROM_ISR(woken);
}

static void dispatch(const yk_key_event_t *events, int n) {
    for (int i = 0; i < n; i++) {
        ESP_LOGD(TAG, "%s at=%lld held=%d", yk_key_event_name(events[i].type), (long long)events[i].at_ms,
                 (int)events[i].held_ms);
        taskENTER_CRITICAL(&s_lock);
        const yk_key_cb_t grab = s_grab.cb;
        void *grab_ctx = s_grab.ctx;
        taskEXIT_CRITICAL(&s_lock);
        if (grab) {
            grab(&events[i], grab_ctx);
            continue;
        }
        for (int k = 0; k < MAX_SUBS; k++) {
            taskENTER_CRITICAL(&s_lock);
            const yk_key_cb_t cb = s_subs[k].cb;
            void *ctx = s_subs[k].ctx;
            taskEXIT_CRITICAL(&s_lock);
            if (cb) cb(&events[i], ctx);
        }
    }
}

static void feed_edge(bool pressed, int64_t at_ms) {
    yk_key_event_t ev[YK_KEY_SM_MAX_EVENTS];
    dispatch(ev, yk_key_sm_edge(&s_sm, pressed, at_ms, ev));
}

/** Acts on the level at once (no delay before DOWN), then reads it again once the contacts settled. */
static void handle_edge(void) {
    const int64_t first = s_edge_us ? s_edge_us / 1000 : now_ms();
    bool moved = false;
    if (level_pressed() != s_sm.pressed) {
        feed_edge(!s_sm.pressed, first);
        moved = true;
    }
    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
    ulTaskNotifyValueClear(NULL, NOTE_EDGE);  // the bounces of this edge
    s_edge_us = 0;
    if (level_pressed() != s_sm.pressed) feed_edge(!s_sm.pressed, moved ? now_ms() : first);
}

static void key_task(void *arg) {
    (void)arg;
    for (;;) {
        int64_t due = yk_key_sm_deadline(&s_sm);
        if (s_sim_release_ms >= 0 && (due < 0 || s_sim_release_ms < due)) due = s_sim_release_ms;
        TickType_t wait = portMAX_DELAY;
        if (due >= 0) {
            const int64_t left = due - now_ms();
            wait = left <= 0 ? 0 : pdMS_TO_TICKS(left) + 1;
        }
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, wait);
        if (bits & NOTE_EDGE) {
            if (s_sim_release_ms < 0) handle_edge();
            else s_edge_us = 0;  // the real key during a simulated press: ignored, and its time with it
        }
        if (s_sim_release_ms >= 0 && now_ms() >= s_sim_release_ms) {
            s_sim_release_ms = -1;
            feed_edge(false, now_ms());
        }
        int hold;  // the next simulated press, once the key is free
        if (s_sim_release_ms < 0 && !s_sm.pressed && xQueueReceive(s_sim, &hold, 0) == pdTRUE) {
            const int64_t t = now_ms();
            s_sim_release_ms = t + hold;
            feed_edge(true, t);
            xTaskNotify(s_task, NOTE_SIM, eSetBits);  // look again after this one
        }
        yk_key_event_t ev[YK_KEY_SM_MAX_EVENTS];
        dispatch(ev, yk_key_sm_tick(&s_sm, now_ms(), ev));
    }
}

esp_err_t yk_key_init(const yk_key_config_t *cfg) {
    if (s_task) return ESP_OK;
    yk_key_sm_init(&s_sm, cfg);
    s_sim = xQueueCreate(4, sizeof(int));
    if (!s_sim) return ESP_ERR_NO_MEM;
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << KEY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;
    if (xTaskCreate(key_task, "yk_key", 4096, NULL, TASK_PRIO, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;  // already installed by someone else
    err = gpio_isr_handler_add(KEY_GPIO, on_edge, NULL);
    if (err != ESP_OK) return err;
    if (level_pressed()) xTaskNotify(s_task, NOTE_EDGE, eSetBits);  // held since boot
    ESP_LOGI(TAG, "side key ready (hold %d ms, double %s)", s_sm.cfg.hold_ms,
             s_sm.cfg.no_double ? "off" : "450 ms");
    return ESP_OK;
}

esp_err_t yk_key_subscribe(yk_key_cb_t cb, void *ctx) {
    esp_err_t err = ESP_ERR_NO_MEM;
    taskENTER_CRITICAL(&s_lock);
    for (int k = 0; k < MAX_SUBS; k++) {
        if (!s_subs[k].cb) {
            s_subs[k].cb = cb;
            s_subs[k].ctx = ctx;
            err = ESP_OK;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return err;
}

void yk_key_grab(yk_key_cb_t cb, void *ctx) {
    taskENTER_CRITICAL(&s_lock);
    s_grab.cb = cb;
    s_grab.ctx = ctx;
    taskEXIT_CRITICAL(&s_lock);
}

bool yk_key_pressed(void) { return level_pressed(); }

bool yk_key_wait_release(int timeout_ms) {
    if (!s_task) {
        // Before yk_key_init() (yaykit_app leaving at start): make the pin readable first.
        gpio_input_enable(KEY_GPIO);
        gpio_pullup_en(KEY_GPIO);
    }
    const int64_t end = now_ms() + timeout_ms;
    while (level_pressed()) {
        if (now_ms() >= end) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

esp_err_t yk_key_simulate(int hold_ms) {
    if (!s_task) return ESP_ERR_INVALID_STATE;
    if (hold_ms <= 0) return ESP_ERR_INVALID_ARG;
    if (xQueueSend(s_sim, &hold_ms, 0) != pdTRUE) return ESP_ERR_NO_MEM;
    xTaskNotify(s_task, NOTE_SIM, eSetBits);
    return ESP_OK;
}

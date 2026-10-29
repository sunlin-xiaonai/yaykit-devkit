// Fast touch (P20): for a timed game, the CST820 read every few milliseconds (or at its interrupt, when the pin is
// wired: CONFIG_YK_BOARD_TOUCH_INT_GPIO) on a task of its own, each press and release queued with its esp_timer
// time. LVGL takes the same reads meanwhile (board.c touch_read_cb), so the bus carries one read per period.
#include "board.h"
#include "board_priv.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "yk_board_logic.h"

static const char *TAG = "touch_fast";

#ifndef CONFIG_YK_BOARD_TOUCH_INT_GPIO
#define CONFIG_YK_BOARD_TOUCH_INT_GPIO -1
#endif

#define EDGES 16

static struct {
    TaskHandle_t task;
    QueueHandle_t edges;
    portMUX_TYPE lock;
    volatile bool on, stop;
    int period_ms;
    bool irq;
    volatile int64_t irq_us;  // when the interrupt came (the press is timed by it, not by the read after)
    bool pressed;
    int32_t x, y;
} F = {.lock = portMUX_INITIALIZER_UNLOCKED};

bool board_touch_fast_point(bool *pressed, int32_t *x, int32_t *y) {
    if (!F.on) return false;
    taskENTER_CRITICAL(&F.lock);
    *pressed = F.pressed, *x = F.x, *y = F.y;
    taskEXIT_CRITICAL(&F.lock);
    return true;
}

#if CONFIG_YK_BOARD_TOUCH_INT_GPIO >= 0
static void IRAM_ATTR on_irq(void *arg) {
    (void)arg;
    F.irq_us = esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    if (F.task) vTaskNotifyGiveFromISR(F.task, &woken);
    portYIELD_FROM_ISR(woken);
}
#endif

static void push(bool pressed, int32_t x, int32_t y, int64_t t) {
    const board_touch_edge_t e = {.t_us = t, .x = (int16_t)x, .y = (int16_t)y, .pressed = pressed};
    if (xQueueSend(F.edges, &e, 0) != pdTRUE) {
        board_touch_edge_t old;
        xQueueReceive(F.edges, &old, 0);  // full: the oldest goes
        xQueueSend(F.edges, &e, 0);
    }
}

static void fast_task(void *arg) {
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    while (!F.stop) {
        int64_t t;
        if (F.irq) {
            // The controller pulls INT low while it has a report; still poll slowly for the release.
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(F.pressed ? F.period_ms : 50));
            t = F.irq_us ? F.irq_us : esp_timer_get_time();
            F.irq_us = 0;
        } else {
            vTaskDelayUntil(&wake, pdMS_TO_TICKS(F.period_ms));
            t = esp_timer_get_time();
        }
        uint8_t b[6] = {0};
        int32_t x = F.x, y = F.y;
        const bool down = board_touch_read(b) == ESP_OK && yk_board_touch_point(b, BOARD_LCD_H_RES, BOARD_LCD_V_RES, &x, &y);
        const bool was = F.pressed;
        taskENTER_CRITICAL(&F.lock);
        F.pressed = down;
        if (down) F.x = x, F.y = y;
        taskEXIT_CRITICAL(&F.lock);
        if (down != was) push(down, F.x, F.y, t);
    }
    F.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t board_touch_fast_start(int period_ms) {
    if (!board_touch_indev()) return ESP_ERR_INVALID_STATE;
    if (F.on) return ESP_OK;
    F.period_ms = period_ms < 2 ? 2 : period_ms > 20 ? 20 : period_ms;
    if (!F.edges && !(F.edges = xQueueCreate(EDGES, sizeof(board_touch_edge_t)))) return ESP_ERR_NO_MEM;
    xQueueReset(F.edges);
    F.pressed = false;
    F.stop = false;
    F.irq = false;
#if CONFIG_YK_BOARD_TOUCH_INT_GPIO >= 0
    const int pin = CONFIG_YK_BOARD_TOUCH_INT_GPIO;
    const gpio_config_t io = {.pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
                              .intr_type = GPIO_INTR_NEGEDGE};
    esp_err_t err = gpio_config(&io);
    if (err == ESP_OK) {
        err = gpio_install_isr_service(0);
        if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;  // already installed (yaykit_key)
    }
    if (err == ESP_OK) err = gpio_isr_handler_add(pin, on_irq, NULL);
    F.irq = err == ESP_OK;
    if (!F.irq) ESP_LOGW(TAG, "interrupt on GPIO%d not available (%s): polling", pin, esp_err_to_name(err));
#endif
    // Above the LVGL task and the audio, below the side key: a read takes ~0.3 ms.
    if (xTaskCreate(fast_task, "touch_fast", 3072, NULL, 11, &F.task) != pdPASS) return ESP_ERR_NO_MEM;
    F.on = true;
    ESP_LOGI(TAG, "every %d ms%s", F.period_ms, F.irq ? ", interrupt" : "");
    return ESP_OK;
}

void board_touch_fast_stop(void) {
    if (!F.on) return;
    F.stop = true;
#if CONFIG_YK_BOARD_TOUCH_INT_GPIO >= 0
    if (F.irq) {
        gpio_isr_handler_remove(CONFIG_YK_BOARD_TOUCH_INT_GPIO);
        if (F.task) xTaskNotifyGive(F.task);
    }
#endif
    for (int i = 0; i < 50 && F.task; i++) vTaskDelay(pdMS_TO_TICKS(2));
    F.on = false;
}

bool board_touch_fast_take(board_touch_edge_t *out) { return F.edges && xQueueReceive(F.edges, out, 0) == pdTRUE; }

void board_touch_fast_clear(void) {
    if (F.edges) xQueueReset(F.edges);
}

bool board_touch_fast_irq(void) { return F.on && F.irq; }

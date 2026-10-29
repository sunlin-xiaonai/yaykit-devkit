#include "yaykit_motion.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "driver/i2c_master.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "yk_motion_core.h"

static const char *TAG = "yk_motion";

#define IMU_ADDR 0x6B
#define REG_WHO 0x00
#define REG_CTRL1 0x02
#define REG_CTRL2 0x03  // accelerometer: full scale, rate
#define REG_CTRL3 0x04  // gyroscope: full scale, rate
#define REG_CTRL7 0x08  // enables
#define REG_STATUS 0x2D  // STATUSINT, STATUS0, STATUS1, TIMESTAMP (3), TEMP (2), AX..GZ (12): 20 bytes from here
#define BURST 20
#define ACC_LSB_PER_G 4096.0f   // +-8 g
#define GYR_LSB_PER_DPS 32.0f   // +-1024 dps
#define NS "yk_pref"
#define CAL_KEY "imu_cal"
#define EVENTS 16
#define STREAM_MAX 16  // samples a batch (20 ms at 224 Hz is 5)

static struct {
    i2c_master_dev_handle_t dev;
    TaskHandle_t task;
    volatile bool run;
    int odr_hz, period_ms;
    QueueHandle_t events;
    yk_motion_event_fn on_event;
    void *ctx;
    SemaphoreHandle_t lock;  // the core and the stream listener
    ykm_core_t *core;        // PSRAM
    yk_motion_cal_t cal;
    yk_motion_thresholds_t th;
    bool th_dirty;
    yk_motion_stream_fn stream;
    void *stream_ctx;
    yk_motion_sample_t batch[STREAM_MAX];
    int batch_n;
    int64_t batch_us;
    float raw_acc[3], raw_gyr[3];
    bool have_raw;
    uint32_t samples, skipped, errors;
    // `motion log`: the task prints the samples itself (the console must not hold the lock)
    volatile int64_t log_until_us;
    uint32_t log_seq;
    volatile int64_t events_until_us;
} M;

static esp_err_t wr(uint8_t reg, uint8_t v) {
    const uint8_t b[2] = {reg, v};
    return i2c_master_transmit(M.dev, b, 2, 50);
}

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n) { return i2c_master_transmit_receive(M.dev, &reg, 1, buf, n, 20); }

static void load_cal(void) {
    yk_motion_default_cal(&M.cal);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    yk_motion_cal_t c;
    size_t len = sizeof(c);
    if (nvs_get_blob(h, CAL_KEY, &c, &len) == ESP_OK && len == sizeof(c)) {
        bool ok = true;
        for (int i = 0; i < 3; i++) ok &= abs(c.axis[i]) >= 1 && abs(c.axis[i]) <= 3;
        if (ok) M.cal = c;
    }
    nvs_close(h);
}

static void deliver(const yk_motion_event_t *ev) {
    if (M.events_until_us && esp_timer_get_time() < M.events_until_us)
        printf("MOTION_EVENT %s t=%lld strength=%.2f angle=%.0f conf=%.2f\n", ykm_kind_name(ev->kind),
               (long long)ev->t_ms, ev->strength, ev->angle, ev->confidence);
    if (M.on_event) M.on_event(ev, M.ctx);
    else if (xQueueSend(M.events, ev, 0) != pdTRUE) {
        yk_motion_event_t old;
        xQueueReceive(M.events, &old, 0);
        xQueueSend(M.events, ev, 0);
    }
}

static void motion_task(void *arg) {
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    uint32_t last_stamp = UINT32_MAX;
    while (M.run) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(M.period_ms));
        uint8_t b[BURST];
        if (rd(REG_STATUS, b, BURST) != ESP_OK) {
            M.errors++;
            continue;
        }
        const uint32_t stamp = b[3] | (b[4] << 8) | ((uint32_t)b[5] << 16);
        if (stamp == last_stamp) {  // no new sample since the last read
            M.skipped++;
            continue;
        }
        last_stamp = stamp;
        const int64_t t = esp_timer_get_time();
        float acc[3], gyr[3];
        for (int i = 0; i < 3; i++) {
            acc[i] = (int16_t)(b[8 + 2 * i] | (b[9 + 2 * i] << 8)) / ACC_LSB_PER_G;
            gyr[i] = (int16_t)(b[14 + 2 * i] | (b[15 + 2 * i] << 8)) / GYR_LSB_PER_DPS;
        }
        M.samples++;
        yk_motion_event_t ev[8];
        int n;
        yk_motion_stream_fn stream;
        void *sctx;
        bool flush = false;
        xSemaphoreTake(M.lock, portMAX_DELAY);
        memcpy(M.raw_acc, acc, sizeof(acc));
        memcpy(M.raw_gyr, gyr, sizeof(gyr));
        M.have_raw = true;
        if (M.th_dirty) {
            ykm_set_thresholds(M.core, &M.th);
            M.th_dirty = false;
        }
        n = ykm_feed(M.core, acc, gyr, t, ev, 8);
        stream = M.stream;
        sctx = M.stream_ctx;
        if (stream) {
            if (M.batch_n < STREAM_MAX) M.batch[M.batch_n++] = M.core->s;
            if (!M.batch_us) M.batch_us = t;
            flush = t - M.batch_us >= 20000 || M.batch_n == STREAM_MAX;
        }
        xSemaphoreGive(M.lock);
        if (flush) {
            stream(M.batch, (size_t)M.batch_n, sctx);
            M.batch_n = 0;
            M.batch_us = 0;
        }
        for (int i = 0; i < n; i++) deliver(&ev[i]);
        if (M.log_until_us) {
            if (t < M.log_until_us) {
                char line[96];
                ykm_csv_format(line, sizeof(line), t, acc, gyr);
                printf("MC %u %s\n", (unsigned)M.log_seq++, line);
            } else {
                M.log_until_us = 0;
                puts("MOTION_CSV_END");
            }
        }
    }
    M.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t yk_motion_start(const yk_motion_config_t *cfg) {
    if (M.run) return ESP_OK;
    if (!board_i2c()) return ESP_ERR_INVALID_STATE;
    const yk_motion_config_t none = {0};
    if (!cfg) cfg = &none;
    if (!M.dev) {
        const i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = IMU_ADDR,
                                        .scl_speed_hz = 400000};
        if (i2c_master_bus_add_device(board_i2c(), &dc, &M.dev) != ESP_OK) return ESP_ERR_NOT_FOUND;
    }
    uint8_t who = 0;
    if (rd(REG_WHO, &who, 1) != ESP_OK || who != 0x05) {
        ESP_LOGW(TAG, "QMI8658 not found (WHO_AM_I 0x%02X)", who);
        i2c_master_bus_rm_device(M.dev);
        M.dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    M.odr_hz = cfg->odr_hz == 56 ? 56 : 224;
    M.period_ms = M.odr_hz == 56 ? 16 : 4;  // a little faster than the sensor: every sample is read once
    const uint8_t odr = M.odr_hz == 56 ? 0x07 : 0x05;  // 56.05 / 224.2 Hz with both sensors on
    if (wr(REG_CTRL1, 0x40) != ESP_OK ||            // address auto-increment, little endian
        wr(REG_CTRL2, 0x20 | odr) != ESP_OK ||      // accelerometer +-8 g
        wr(REG_CTRL3, 0x60 | odr) != ESP_OK ||      // gyroscope +-1024 dps
        wr(REG_CTRL7, 0x03) != ESP_OK)              // both on
        return ESP_FAIL;
    if (!M.lock && !(M.lock = xSemaphoreCreateMutex())) return ESP_ERR_NO_MEM;
    if (!M.events && !(M.events = xQueueCreate(EVENTS, sizeof(yk_motion_event_t)))) return ESP_ERR_NO_MEM;
    if (!M.core && !(M.core = heap_caps_calloc(1, sizeof(ykm_core_t), MALLOC_CAP_SPIRAM))) return ESP_ERR_NO_MEM;
    load_cal();
    if (cfg->thresholds) M.th = *cfg->thresholds;
    else yk_motion_default_thresholds(&M.th);
    ykm_init(M.core, &M.th, &M.cal);
    M.on_event = cfg->on_event;
    M.ctx = cfg->ctx;
    xQueueReset(M.events);
    M.run = true;
    // Above the LVGL task and the audio front end; reads take ~0.5 ms of I2C.
    if (xTaskCreatePinnedToCore(motion_task, "yk_motion", 4096, NULL, 9, &M.task, 0) != pdPASS) {
        M.run = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "QMI8658 at %d Hz, axes %d %d %d", M.odr_hz, M.cal.axis[0], M.cal.axis[1], M.cal.axis[2]);
    return ESP_OK;
}

void yk_motion_stop(void) {
    if (!M.run) return;
    M.run = false;
    for (int i = 0; i < 50 && M.task; i++) vTaskDelay(pdMS_TO_TICKS(5));
    wr(REG_CTRL7, 0x00);  // sensors off
}

bool yk_motion_running(void) { return M.run; }

bool yk_motion_take(yk_motion_event_t *ev) { return M.events && xQueueReceive(M.events, ev, 0) == pdTRUE; }

void yk_motion_state(yk_motion_state_t *out) {
    if (!M.core || !M.lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(M.lock, portMAX_DELAY);
    ykm_state(M.core, out);
    xSemaphoreGive(M.lock);
}

void yk_motion_stream(yk_motion_stream_fn fn, void *ctx) {
    if (!M.lock) {
        M.stream = fn, M.stream_ctx = ctx;
        return;
    }
    xSemaphoreTake(M.lock, portMAX_DELAY);
    M.stream = fn;
    M.stream_ctx = ctx;
    M.batch_n = 0;
    M.batch_us = 0;
    xSemaphoreGive(M.lock);
}

bool yk_motion_holder(int window_ms, float *angle, float *confidence) {
    if (!M.core || !M.lock) return false;
    xSemaphoreTake(M.lock, portMAX_DELAY);
    const bool ok = ykm_holder(M.core, M.core->t_us / 1000, window_ms, angle, confidence);
    xSemaphoreGive(M.lock);
    return ok;
}

esp_err_t yk_motion_set_thresholds(const yk_motion_thresholds_t *th) {
    if (!th) return ESP_ERR_INVALID_ARG;
    if (!M.lock) {
        M.th = *th;
        return ESP_OK;
    }
    xSemaphoreTake(M.lock, portMAX_DELAY);
    M.th = *th;
    M.th_dirty = true;
    xSemaphoreGive(M.lock);
    return ESP_OK;
}

void yk_motion_calibration(yk_motion_cal_t *out) {
    if (!M.lock) load_cal();
    *out = M.cal;
}

esp_err_t yk_motion_save_calibration(const yk_motion_cal_t *cal) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, CAL_KEY, cal, sizeof(*cal));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        M.cal = *cal;
        if (M.lock) {
            xSemaphoreTake(M.lock, portMAX_DELAY);
            M.core->cal = *cal;
            xSemaphoreGive(M.lock);
        }
    }
    return err;
}

bool yk_motion_raw(float acc[3], float gyr[3]) {
    if (!M.have_raw || !M.lock) return false;
    xSemaphoreTake(M.lock, portMAX_DELAY);
    memcpy(acc, M.raw_acc, sizeof(M.raw_acc));
    memcpy(gyr, M.raw_gyr, sizeof(M.raw_gyr));
    xSemaphoreGive(M.lock);
    return true;
}

// ---------------------------------------------------------------- console

static struct {
    int8_t up;       // the sensor axis that reads +1 g lying flat (glass up): the device's z
    int8_t top;      // standing upright: the device's y
    float gyro[3];   // sensor axes, at rest
} s_cal;

/** The sensor axis (+-1..3) that carries most of the average reading over ms. */
static int8_t dominant_axis(int ms, float gyro_avg[3]) {
    float a[3] = {0}, g[3] = {0}, acc[3], gyr[3];
    int n = 0;
    for (int64_t end = esp_timer_get_time() + ms * 1000LL; esp_timer_get_time() < end; n++) {
        if (yk_motion_raw(acc, gyr))
            for (int i = 0; i < 3; i++) a[i] += acc[i], g[i] += gyr[i];
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    int k = 0;
    for (int i = 1; i < 3; i++)
        if (fabsf(a[i]) > fabsf(a[k])) k = i;
    if (gyro_avg)
        for (int i = 0; i < 3; i++) gyro_avg[i] = n ? g[i] / n : 0;
    printf("MOTION_CAL axis=%c%c avg=%.3f,%.3f,%.3f g\n", a[k] < 0 ? '-' : '+', 'x' + k, n ? a[0] / n : 0,
           n ? a[1] / n : 0, n ? a[2] / n : 0);
    return (int8_t)((a[k] < 0 ? -1 : 1) * (k + 1));
}

/** x = y cross z for signed axes. */
static int8_t cross_axis(int8_t y, int8_t z) {
    float vy[3] = {0}, vz[3] = {0};
    vy[abs(y) - 1] = y < 0 ? -1 : 1;
    vz[abs(z) - 1] = z < 0 ? -1 : 1;
    const float x[3] = {vy[1] * vz[2] - vy[2] * vz[1], vy[2] * vz[0] - vy[0] * vz[2], vy[0] * vz[1] - vy[1] * vz[0]};
    for (int i = 0; i < 3; i++)
        if (x[i] != 0) return (int8_t)((x[i] < 0 ? -1 : 1) * (i + 1));
    return 0;
}

static int motion_cmd(int argc, char **argv) {
    if (!M.run) {
        puts("MOTION not running");
        return 1;
    }
    const char *sub = argc > 1 ? argv[1] : "";
    if (!strcmp(sub, "log")) {
        const int s = argc > 2 ? atoi(argv[2]) : 5;
        M.log_seq = 0;
        printf("MOTION_CSV_BEGIN %d Hz %s\n", M.odr_hz, YKM_CSV_HEADER);
        M.log_until_us = esp_timer_get_time() + (int64_t)(s > 0 && s <= 120 ? s : 5) * 1000000;
        while (M.log_until_us) vTaskDelay(pdMS_TO_TICKS(50));
        return 0;
    }
    if (!strcmp(sub, "events")) {
        const int s = argc > 2 ? atoi(argv[2]) : 60;
        M.events_until_us = esp_timer_get_time() + (int64_t)s * 1000000;
        printf("MOTION_EVENTS for %d s\n", s);
        return 0;
    }
    if (!strcmp(sub, "cal") && argc > 2) {
        yk_motion_cal_t cal = M.cal;
        if (!strcmp(argv[2], "flat")) {
            s_cal.up = dominant_axis(1000, s_cal.gyro);
            printf("MOTION_CAL flat: up is sensor %c%c; now stand it upright, screen reading normally: motion cal upright\n",
                   s_cal.up < 0 ? '-' : '+', 'x' + abs(s_cal.up) - 1);
            return 0;
        }
        if (!strcmp(argv[2], "upright")) {
            if (!s_cal.up) {
                puts("MOTION_CAL first: motion cal flat");
                return 1;
            }
            s_cal.top = dominant_axis(1000, NULL);
            if (abs(s_cal.top) == abs(s_cal.up)) {
                puts("MOTION_CAL upright reads the same axis as flat: stand it on its edge");
                return 1;
            }
            cal.axis[2] = s_cal.up;
            cal.axis[1] = s_cal.top;
            cal.axis[0] = cross_axis(s_cal.top, s_cal.up);
            float g[3];
            ykm_map(&cal, s_cal.gyro, g);
            memcpy(cal.gyro_bias, g, sizeof(g));
            M.cal = cal;
            xSemaphoreTake(M.lock, portMAX_DELAY);
            M.core->cal = cal;
            xSemaphoreGive(M.lock);
            printf("MOTION_CAL axes %d %d %d (in use; motion cal save keeps it)\n", cal.axis[0], cal.axis[1], cal.axis[2]);
            return 0;
        }
        if (!strcmp(argv[2], "save")) {
            const esp_err_t err = yk_motion_save_calibration(&M.cal);
            printf("MOTION_CAL save %s\n", esp_err_to_name(err));
            return err == ESP_OK ? 0 : 1;
        }
        if (!strcmp(argv[2], "reset")) {
            nvs_handle_t h;
            if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_key(h, CAL_KEY);  // only this key of the shared namespace
                nvs_commit(h);
                nvs_close(h);
            }
            load_cal();
            xSemaphoreTake(M.lock, portMAX_DELAY);
            M.core->cal = M.cal;
            xSemaphoreGive(M.lock);
            printf("MOTION_CAL reset: axes %d %d %d\n", M.cal.axis[0], M.cal.axis[1], M.cal.axis[2]);
            return 0;
        }
    }
    yk_motion_state_t st;
    yk_motion_state(&st);
    float acc[3] = {0}, gyr[3] = {0};
    yk_motion_raw(acc, gyr);
    xSemaphoreTake(M.lock, portMAX_DELAY);
    const bool settled = M.core->settled;
    const float rest_g = M.core->rest_g_len, *b = M.core->cal.gyro_bias;
    printf("MOTION_REST settled=%d rest_g=%.4f gyro_bias=%.2f,%.2f,%.2f\n", settled, rest_g, b[0], b[1], b[2]);
    xSemaphoreGive(M.lock);
    printf("MOTION raw_acc=%.3f,%.3f,%.3f raw_gyr=%.1f,%.1f,%.1f gravity=%.2f,%.2f,%.2f lin=%.3f,%.3f,%.3f tilt=%.0f "
           "up=%.0f flat=%d upright=%d face_down=%d still=%d moving=%d shaking=%d strength=%.2f holder=%.0f/%.2f "
           "samples=%u skipped=%u errors=%u hz=%d axes=%d,%d,%d\n",
           acc[0], acc[1], acc[2], gyr[0], gyr[1], gyr[2], st.now.gravity[0], st.now.gravity[1], st.now.gravity[2],
           st.now.lin[0], st.now.lin[1], st.now.lin[2], st.tilt_deg, st.up_angle, st.flat, st.upright, st.face_down,
           st.still, st.moving, st.shaking, st.shake_strength, st.holder_angle, st.holder_confidence,
           (unsigned)M.samples, (unsigned)M.skipped, (unsigned)M.errors, M.odr_hz, M.cal.axis[0], M.cal.axis[1],
           M.cal.axis[2]);
    return 0;
}

esp_err_t yk_motion_register_console(void) {
    const esp_console_cmd_t cmd = {
        .command = "motion",
        .help = "motion | log <s> | events [s] | cal flat|upright|save|reset",
        .func = motion_cmd,
    };
    return esp_console_cmd_register(&cmd);
}

#include "yaykit_voice.h"

#include <string.h>

#include "audio.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "yk_wav.h"

#define RATE AUDIO_SAMPLE_RATE
#define SLOTS AUDIO_MIC_SLOTS
#define MIC_A 0  // slot 1 is the speaker echo reference
#define MIC_B 2
#define FRAME 480  // 20 ms
#define GAIN_DB 30.0f

static const char *TAG = "yk_voice";
static uint8_t *wav;          // header + samples, PSRAM
static int16_t *raw;          // one TDM frame, PSRAM
static size_t capacity;       // sample bytes
static size_t recorded;       // sample bytes
static volatile bool stop_requested, recording, failed;
static volatile uint8_t level;
static SemaphoreHandle_t finished;

esp_err_t yk_voice_init(int max_seconds) {
    if (wav) return ESP_OK;
    esp_err_t err = audio_init();
    if (err != ESP_OK) return err;
    if (audio_sample_rate() != RATE) {  // the WAV and the codec are opened at RATE
        ESP_LOGE(TAG, "audio runs at %u Hz, not %d", (unsigned)audio_sample_rate(), RATE);
        return ESP_ERR_INVALID_STATE;
    }
    capacity = (size_t)max_seconds * RATE * 2;
    wav = heap_caps_malloc(YK_WAV_HEADER_BYTES + capacity, MALLOC_CAP_SPIRAM);
    raw = heap_caps_malloc(FRAME * SLOTS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    finished = xSemaphoreCreateBinary();
    if (!wav || !raw || !finished) return ESP_ERR_NO_MEM;
    xSemaphoreGive(finished);
    return ESP_OK;
}

static void capture_task(void *arg) {
    (void)arg;
    esp_codec_dev_handle_t mic = audio_mic();
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = RATE,
        .channel = SLOTS,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) |
                        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3),
        .bits_per_sample = 16,
    };
    esp_log_level_set("i2s_common", ESP_LOG_NONE);  // "channel not enabled" on reopen is harmless
    bool open = mic && esp_codec_dev_open(mic, &fs) == ESP_CODEC_DEV_OK;
    esp_log_level_set("i2s_common", ESP_LOG_ERROR);
    if (open) esp_codec_dev_set_in_gain(mic, GAIN_DB);
    failed = !open;
    int16_t *out = (int16_t *)(wav + YK_WAV_HEADER_BYTES);
    while (open && !stop_requested && recorded + FRAME * 2 <= capacity) {
        if (esp_codec_dev_read(mic, (uint8_t *)raw, FRAME * SLOTS * 2) != ESP_CODEC_DEV_OK) {
            failed = true;
            break;
        }
        int16_t *dst = out + recorded / 2;
        for (int i = 0; i < FRAME; i++) dst[i] = (int16_t)(((int32_t)raw[i * SLOTS + MIC_A] + raw[i * SLOTS + MIC_B]) / 2);
        level = yk_wav_level(dst, FRAME);
        recorded += FRAME * 2;
    }
    if (open) esp_codec_dev_close(mic);
    yk_wav_header(wav, RATE, (uint32_t)recorded);
    level = 0;
    recording = false;
    ESP_LOGI(TAG, "recorded %.2f s%s", recorded / (RATE * 2.0f), failed ? " (microphone failed)" : "");
    xSemaphoreGive(finished);
    vTaskDelete(NULL);
}

esp_err_t yk_voice_start(void) {
    if (!wav) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(finished, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;  // still recording
    recorded = 0;
    stop_requested = false;
    failed = false;
    recording = true;
    // Above LVGL (6) so drawing never makes the microphone drop samples.
    if (xTaskCreate(capture_task, "yk_voice", 4096, NULL, 7, NULL) != pdPASS) {
        recording = false;
        xSemaphoreGive(finished);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void yk_voice_stop(void) { stop_requested = true; }

bool yk_voice_recording(void) { return recording; }

uint8_t yk_voice_level(void) { return level; }

const uint8_t *yk_voice_wav(size_t *len, float *seconds) {
    if (!wav) return NULL;
    xSemaphoreTake(finished, portMAX_DELAY);
    xSemaphoreGive(finished);
    if (len) *len = YK_WAV_HEADER_BYTES + recorded;
    if (seconds) *seconds = recorded / (RATE * 2.0f);
    return failed ? NULL : wav;
}

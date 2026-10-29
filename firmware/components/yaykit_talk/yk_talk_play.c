// The reply's playback (the main firmware's yay_play): packets are queued without waiting and a task of their own
// hands them to the speaker, waiting for room as long as they are still wanted. A new utterance, a stop or a cue
// makes everything queued before it stale (epochs): a press stops Yay at once, however much was queued.
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "yaykit_speak.h"
#include "yaykit_talk.h"
#include "yk_speak_dsp.h"

static const char *TAG = "yk_talk_play";

#define PLAY_LEN 96  // 5.8 s of 60 ms packets, on top of the speaker's own buffer
#define PACKET_MAX 400

typedef struct {
    uint32_t epoch;  // s_epoch when it was queued
    bool end;        // no packet: the utterance's end, given after everything before it
    uint16_t bytes;
    uint8_t packet[PACKET_MAX];
} play_t;

static QueueHandle_t s_play;
static play_t *s_item, *s_task_item;  // the callers' (one at a time, under s_spk) and the task's, PSRAM
// s_epoch counts utterances and cut-offs; s_end_epoch is set by the task once it gave the end. s_spk is held around
// every speaker call that starts, stops or feeds an utterance, so a packet checked against one epoch can never land
// in the next utterance.
static volatile uint32_t s_epoch, s_end_epoch;
static SemaphoreHandle_t s_spk;
static int16_t *s_cue[2];  // start, end: made once, PSRAM
static size_t s_cue_n[2];

static void play_task(void *arg) {
    (void)arg;
    play_t *p = s_task_item;
    for (;;) {
        if (xQueueReceive(s_play, p, portMAX_DELAY) != pdTRUE) continue;
        for (;;) {
            xSemaphoreTake(s_spk, portMAX_DELAY);  // held for one try, never while waiting
            const bool wanted = p->epoch == s_epoch;
            esp_err_t err = ESP_OK;
            if (wanted && p->end) {
                yk_speak_end();
                s_end_epoch = p->epoch;
            } else if (wanted) {
                err = yk_speak_opus_wait(p->packet, p->bytes, 0);
            }
            xSemaphoreGive(s_spk);
            if (!wanted || err != ESP_ERR_TIMEOUT) break;  // given, stale, or the speaker stopped
            vTaskDelay(pdMS_TO_TICKS(10));                 // full: the speaker's buffer is waiting already
        }
    }
}

esp_err_t yk_talk_play_init(void) {
    if (s_play) return ESP_OK;
    s_spk = xSemaphoreCreateMutex();
    s_item = heap_caps_calloc(1, sizeof(play_t), MALLOC_CAP_SPIRAM);
    s_task_item = heap_caps_calloc(1, sizeof(play_t), MALLOC_CAP_SPIRAM);
    for (int i = 0; i < 2; i++) {
        s_cue[i] = heap_caps_malloc(YK_SPEAK_RATE / 5 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (s_cue[i])
            s_cue_n[i] = yk_chime_synth(i ? YK_CHIME_END : YK_CHIME_START, YK_SPEAK_RATE, s_cue[i], YK_SPEAK_RATE / 5);
    }
    QueueHandle_t q = xQueueCreateWithCaps(PLAY_LEN, sizeof(play_t), MALLOC_CAP_SPIRAM);
    if (!s_spk || !s_item || !s_task_item || !q) return ESP_ERR_NO_MEM;
    s_play = q;
    // Core 1 with the speaker; it only waits for room, so a PSRAM stack does.
    if (xTaskCreatePinnedToCoreWithCaps(play_task, "yk_talk_play", 3072, NULL, 6, NULL, 1, MALLOC_CAP_SPIRAM) !=
        pdPASS) {
        s_play = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void yk_talk_play_begin(void) {
    if (!s_play) return;
    xSemaphoreTake(s_spk, portMAX_DELAY);
    s_epoch++;  // a new utterance: anything queued is an older one's
    yk_speak_begin();
    xSemaphoreGive(s_spk);
}

static bool queue(const uint8_t *packet, size_t bytes, bool end) {
    if (!s_play || bytes > PACKET_MAX) return false;
    xSemaphoreTake(s_spk, portMAX_DELAY);  // s_item is shared by the callers
    s_item->epoch = s_epoch;
    s_item->end = end;
    s_item->bytes = (uint16_t)bytes;
    if (bytes) memcpy(s_item->packet, packet, bytes);
    xSemaphoreGive(s_spk);
    // The end waits for room (it must come); a packet never does.
    const bool ok = xQueueSend(s_play, s_item, end ? pdMS_TO_TICKS(1000) : 0) == pdTRUE;
    if (!ok && end) ESP_LOGW(TAG, "the end was not queued");
    return ok;
}

bool yk_talk_play_opus(const uint8_t *opus, size_t bytes) { return queue(opus, bytes, false); }

void yk_talk_play_end(void) { queue(NULL, 0, true); }

void yk_talk_play_stop(void) {
    if (!s_play) return;
    xSemaphoreTake(s_spk, portMAX_DELAY);
    s_epoch++;
    yk_speak_cancel();  // fades out by itself; waiting for it (~150 ms) would cost the preroll
    xSemaphoreGive(s_spk);
}

void yk_talk_play_cue(bool start) {
    if (!s_play || !s_cue[start ? 0 : 1]) return;
    // Not yk_speak_chime(): that waits until it has been heard, and the loop must not.
    const int i = start ? 0 : 1;
    xSemaphoreTake(s_spk, portMAX_DELAY);
    s_epoch++;
    if (yk_speak_begin() == ESP_OK) {
        yk_speak_pcm(s_cue[i], s_cue_n[i]);
        yk_speak_end();
    }
    xSemaphoreGive(s_spk);
}

bool yk_talk_play_done(void) { return s_end_epoch == s_epoch && !yk_speak_busy(); }

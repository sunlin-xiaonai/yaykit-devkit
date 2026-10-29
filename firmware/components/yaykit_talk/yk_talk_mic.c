// The microphones' side of a turn (the main firmware's yay_mic.c, with the session taken out into a sink): the front
// end runs from key down into a ring of that press's own; from the stream's start everything in it (the preroll) and
// then the live audio go to the sink as PCM and/or Opus, 60 ms a piece; on release the last samples are padded to a
// piece and the sink's end follows. One task (core 0; libopus needs a 25 KB stack, kept in PSRAM) does all of it, and
// sends the session's other messages too (yk_talk_mic_post_send), in the order they were given: the network never
// waits in the conversation loop, and a turn's messages never overtake each other.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "yaykit_audio.h"
#include "yaykit_session.h"
#include "yaykit_speak.h"
#include "yaykit_talk.h"
#include "yk_speak_dsp.h"
#include "yk_talk_flush.h"
#include "yk_talk_internal.h"

static const char *TAG = "yk_talk_mic";

#define RINGS 2      // a press while the stream before still goes out gets the other ring
#define RING 32768   // samples: 2 s, far more than the encoder ever falls behind
#define PIECE 960    // 60 ms
#define STACK (32 * 1024)

typedef enum { CMD_STREAM, CMD_STOP, CMD_CANCEL, CMD_DISCARD, CMD_SEND } cmd_type_t;
typedef struct {
    cmd_type_t type;
    uint8_t ring;
    uint32_t gen;  // the ring's generation when the command was given: a newer press may have taken it since
    char tag[YK_TALK_TURN_LEN + 1];
    int64_t at_us;
    uint32_t seq;         // its number in the uplink (yk_flush_t), for yk_talk_flush
    yk_talk_sink_t sink;  // CMD_STREAM
    char *json;           // CMD_SEND
} cmd_t;

static struct {
    TaskHandle_t task;
    QueueHandle_t cmds;
    SemaphoreHandle_t post_lock;  // numbering and queueing a command (yk_flush_t.posted)
    yk_flush_t flush;             // `posted` under post_lock; `done`, `failed` by the task alone (single words)
    portMUX_TYPE lock;
    struct {
        yk_ring_t ring;
        uint32_t gen;
    } r[RINGS];
    volatile int cur;  // the ring the front end writes into
    yk_opus_enc_t *enc;
    int16_t *piece;
    volatile bool capturing;
    // the task's own
    bool streaming;
    int sring;
    uint32_t sgen;
    char tag[YK_TALK_TURN_LEN + 1];
    yk_talk_sink_t sink;
    uint32_t packets;
    volatile uint8_t level;
} M = {.lock = portMUX_INITIALIZER_UNLOCKED};

static void on_frame(const int16_t *pcm, size_t n, bool speech, uint8_t level, void *ctx) {
    (void)speech, (void)ctx;
    if (!M.capturing) return;
    taskENTER_CRITICAL(&M.lock);
    yk_ring_write(&M.r[M.cur].ring, pcm, n);
    taskEXIT_CRITICAL(&M.lock);
    M.level = level;
    if (M.task) xTaskNotifyGive(M.task);
}

/** Samples waiting in ring i, or 0 when a newer press has taken it. */
static size_t avail(int i, uint32_t gen) {
    taskENTER_CRITICAL(&M.lock);
    const size_t n = M.r[i].gen == gen ? yk_ring_avail(&M.r[i].ring) : 0;
    taskEXIT_CRITICAL(&M.lock);
    return n;
}

/** Up to n samples of ring i (still this generation) into out; returns how many. */
static size_t take(int i, uint32_t gen, int16_t *out, size_t n) {
    taskENTER_CRITICAL(&M.lock);
    const size_t got = M.r[i].gen == gen ? yk_ring_read(&M.r[i].ring, out, n) : 0;
    taskEXIT_CRITICAL(&M.lock);
    return got;
}

static bool fresh(int i, uint32_t gen) {
    taskENTER_CRITICAL(&M.lock);
    const bool same = M.r[i].gen == gen;
    taskEXIT_CRITICAL(&M.lock);
    return same;
}

static void drop(int i, uint32_t gen) {
    taskENTER_CRITICAL(&M.lock);
    if (M.r[i].gen == gen) yk_ring_clear(&M.r[i].ring);
    taskEXIT_CRITICAL(&M.lock);
}

static void on_packet(const uint8_t *packet, size_t bytes, void *ctx) {
    (void)ctx;
    M.packets++;
    if (M.sink.packet) M.sink.packet(packet, bytes, M.sink.ctx);
}

static void give(const int16_t *pcm) {
    if (M.sink.pcm) M.sink.pcm(pcm, PIECE, M.sink.ctx);
    if (M.sink.packet) yk_opus_enc_feed(M.enc, pcm, PIECE, on_packet, NULL);
}

/** Everything whole in the streaming ring goes out. */
static void pump(void) {
    while (avail(M.sring, M.sgen) >= PIECE && take(M.sring, M.sgen, M.piece, PIECE) == PIECE) give(M.piece);
}

static void finish(yk_talk_mic_end_t how) {
    if (M.sink.end) M.sink.end(M.tag, how, M.sink.ctx);
    M.streaming = false;
}

static void run(const cmd_t *c) {
    const bool mine = M.streaming && M.sring == c->ring && M.sgen == c->gen;
    switch (c->type) {
    case CMD_STREAM: {
        if (M.streaming) finish(YK_TALK_MIC_DROPPED);  // a stream never ends without its sink hearing of it
        if (!fresh(c->ring, c->gen)) break;           // a newer press already took this ring
        snprintf(M.tag, sizeof(M.tag), "%s", c->tag);
        M.sink = c->sink;
        // More than the contract's 1000 ms recorded (a slow hold, a busy loop): the oldest goes.
        int drop_ms = 0;
        const int preroll = yk_tsm_preroll((int)(avail(c->ring, c->gen) * 1000 / YK_AUDIO_RATE), &drop_ms);
        for (size_t extra = (size_t)drop_ms * YK_AUDIO_RATE / 1000; extra;) {
            const size_t n = extra < PIECE ? extra : PIECE;
            if (!take(c->ring, c->gen, M.piece, n)) break;
            extra -= n;
        }
        yk_opus_enc_reset(M.enc);
        M.packets = 0;
        if (M.sink.begin && !M.sink.begin(M.tag, preroll, M.sink.ctx)) break;
        M.streaming = true;
        M.sring = c->ring;
        M.sgen = c->gen;
        break;
    }
    case CMD_STOP:
        if (mine) {
            pump();
            const size_t rest = avail(M.sring, M.sgen);
            if (rest) {  // the last piece, padded with silence to a whole one
                memset(M.piece, 0, PIECE * sizeof(int16_t));
                take(M.sring, M.sgen, M.piece, rest < PIECE ? rest : PIECE);
                give(M.piece);
            }
            printf("YK_TALK turn id=%s stage=stopped packets=%u stop_ms=%u\n", M.tag, (unsigned)M.packets,
                   (unsigned)((esp_timer_get_time() - c->at_us) / 1000));
            finish(YK_TALK_MIC_STOPPED);
        }
        drop(c->ring, c->gen);
        break;
    case CMD_CANCEL:
        if (mine) finish(YK_TALK_MIC_CANCELLED);
        drop(c->ring, c->gen);
        break;
    case CMD_DISCARD:
        if (mine) finish(YK_TALK_MIC_DROPPED);
        drop(c->ring, c->gen);
        break;
    case CMD_SEND:
        yk_flush_through(&M.flush, c->seq, yk_session_send(c->json) == ESP_OK);  // frees it
        return;
    }
    yk_flush_through(&M.flush, c->seq, true);  // only this task writes `done` and `failed`
}

static void mic_task(void *arg) {
    (void)arg;
    for (;;) {
        cmd_t c;
        if (xQueueReceive(M.cmds, &c, M.streaming ? 0 : pdMS_TO_TICKS(100)) == pdTRUE) {
            run(&c);
            continue;
        }
        if (M.streaming) {
            pump();
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(40));
        }
    }
}

esp_err_t yk_talk_mic_init(void) {
    if (M.task) return ESP_OK;
    for (int i = 0; i < RINGS; i++) {
        int16_t *ring = heap_caps_malloc(RING * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!ring) return ESP_ERR_NO_MEM;
        yk_ring_init(&M.r[i].ring, ring, RING);
    }
    M.piece = heap_caps_malloc(PIECE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    heap_caps_malloc_extmem_enable(32);  // libopus's encoder state (and its set-up) into PSRAM
    M.enc = yk_opus_enc_create(1, 16000);
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    M.cmds = xQueueCreateWithCaps(24, sizeof(cmd_t), MALLOC_CAP_SPIRAM);
    M.post_lock = xSemaphoreCreateMutex();
    if (!M.piece || !M.enc || !M.cmds || !M.post_lock) return ESP_ERR_NO_MEM;
    // Above the conversation loop (7): listen.stop goes out as soon as it is given.
    if (xTaskCreatePinnedToCoreWithCaps(mic_task, "yk_talk_mic", STACK, NULL, 8, &M.task, 0, MALLOC_CAP_SPIRAM) !=
        pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static bool post(cmd_type_t type, const char *tag, const yk_talk_sink_t *sink, char *json) {
    if (!M.task) {
        free(json);
        return false;
    }
    cmd_t c = {.type = type, .at_us = esp_timer_get_time(), .json = json};
    if (sink) c.sink = *sink;
    taskENTER_CRITICAL(&M.lock);
    c.ring = (uint8_t)M.cur;
    c.gen = M.r[M.cur].gen;
    taskEXIT_CRITICAL(&M.lock);
    snprintf(c.tag, sizeof(c.tag), "%s", tag ? tag : "");
    // Numbered and queued under one lock (posts come from more than one task): the numbers keep the queue's order.
    // Never dropped while the queue has room within a second; the queue only backs up if the network stalls.
    xSemaphoreTake(M.post_lock, portMAX_DELAY);
    c.seq = M.flush.posted + 1;
    const bool queued = xQueueSend(M.cmds, &c, pdMS_TO_TICKS(1000)) == pdTRUE;
    if (queued) M.flush.posted = c.seq;
    xSemaphoreGive(M.post_lock);
    if (!queued) {
        ESP_LOGE(TAG, "uplink queue full: a message was dropped");
        free(json);
        return false;
    }
    xTaskNotifyGive(M.task);
    return true;
}

/** True when the microphones were open (and are closed now). */
static bool capture_off(void) {
    if (!M.capturing) return false;
    yk_audio_stop();  // returns once the microphones are closed: nothing more arrives after this
    M.capturing = false;
    yk_speak_mic(false);
    return true;
}

void yk_talk_mic_open(void) {
    if (!M.task) return;
    capture_off();
    // A fresh ring for this press: the stream before may still be going out of its own.
    taskENTER_CRITICAL(&M.lock);
    M.cur = (M.cur + 1) % RINGS;
    M.r[M.cur].gen++;
    yk_ring_clear(&M.r[M.cur].ring);
    taskEXIT_CRITICAL(&M.lock);
    const yk_audio_config_t cfg = {.mode = YK_AUDIO_PUSH_TO_TALK, .noise_suppression = true, .voice_activity = true,
                                   .on_frame = on_frame};
    M.capturing = true;
    yk_speak_mic(true);  // before they open: no reclaim of the speaker's format under them (yk_speak_reclaim)
    if (yk_audio_start(&cfg) != ESP_OK) {
        M.capturing = false;
        yk_speak_mic(false);
        ESP_LOGE(TAG, "microphones did not open");
    }
}

void yk_talk_mic_stream(const char *tag, const yk_talk_sink_t *sink) { post(CMD_STREAM, tag, sink, NULL); }

void yk_talk_mic_stop(void) {
    capture_off();
    post(CMD_STOP, NULL, NULL, NULL);
}

// Nothing plays after these (a press that did not talk): the I2S's DMA buffers the microphones enlarged go back
// now (yk_speak_reclaim, about 10 KB of internal RAM), not at the next reply.
void yk_talk_mic_cancel(void) {
    if (capture_off()) yk_speak_reclaim();
    post(CMD_CANCEL, NULL, NULL, NULL);
}

void yk_talk_mic_discard(void) {
    if (capture_off()) yk_speak_reclaim();
    post(CMD_DISCARD, NULL, NULL, NULL);
}

uint8_t yk_talk_mic_level(void) { return M.capturing ? M.level : 0; }

bool yk_talk_mic_post_send(char *json) { return json && post(CMD_SEND, NULL, NULL, json); }

static yk_flush_t flush_read(void *ctx) {
    (void)ctx;
    xSemaphoreTake(M.post_lock, portMAX_DELAY);  // `posted` as of now; `done` and `failed` are single words
    const yk_flush_t f = {.posted = M.flush.posted, .done = M.flush.done, .failed = M.flush.failed};
    xSemaphoreGive(M.post_lock);
    return f;
}

static int64_t flush_now(void *ctx) {
    (void)ctx;
    return esp_timer_get_time() / 1000;
}

static void flush_sleep(uint32_t ms, void *ctx) {
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}

esp_err_t yk_talk_mic_flush(uint32_t timeout_ms) {
    if (!M.task) return ESP_ERR_INVALID_STATE;
    const int64_t t0 = esp_timer_get_time();
    const yk_flush_env_t env = {.read = flush_read, .now_ms = flush_now, .sleep_ms = flush_sleep};
    const yk_flush_result_t r = yk_flush_wait(&env, timeout_ms);
    printf("YK_TALK flush=%s ms=%d\n", yk_flush_result_name(r), (int)((esp_timer_get_time() - t0) / 1000));
    return r == YK_FLUSH_SENT ? ESP_OK : r == YK_FLUSH_FAILED ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

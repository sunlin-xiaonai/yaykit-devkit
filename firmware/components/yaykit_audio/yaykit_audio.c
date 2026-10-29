#include "yaykit_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_nsn_models.h"
#include "sdkconfig.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_vadn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "model_path.h"
#include "yk_ptt.h"
#include "yk_srmodels.h"

static const char *TAG = "yk_audio";
extern const esp_nsn_iface_t esp_nsnet2_quantized;  // libnsnet.a

#define I2S_RATE    YK_AUDIO_RATE    // the port runs at 16 kHz: no resampling (P0 6)
#define FRAME       (I2S_RATE / 50)  // 20 ms: one I2S read
#define SLOTS       4
#define FIFO_MAX    2048  // samples between the stages (model chunks are at most 512)
#define CHUNK_MAX   1024  // largest model chunk accepted
#define PREROLL     (YK_AUDIO_RATE * YK_AUDIO_PREROLL_MS / 1000)
#define TASK_CORE   1
#define TASK_PRIO   7     // under playback (8) and the side key (12), over LVGL (6)

typedef struct {
    int16_t d[FIFO_MAX];
    size_t n;
} fifo_t;

static struct {
    bool inited, models;
    TaskHandle_t task;
    SemaphoreHandle_t stopped;
    volatile bool run, running;
    yk_audio_config_t cfg;
    yk_fe_t *fe;
    int16_t *tdm;               // one 20 ms read, PSRAM
    fifo_t *ns_in, *vad_in;     // PSRAM
    // Working buffers in PSRAM: internal RAM is what Wi-Fi, TLS and the models' own state need.
    float *fbuf;                                        // CHUNK_MAX
    int16_t *chunk, *ns_buf, *vad_buf, *combined;       // CHUNK_MAX, CHUNK_MAX, CHUNK_MAX, FRAME
    // nsnet2 and vadnet
    void *models_buf;
    const esp_nsn_iface_t *ns;
    esp_nsn_data_t *ns_data;
    const esp_vadn_iface_t *vad;
    model_iface_data_t *vad_data;
    int ns_chunk, vad_chunk;
    bool speech;
    volatile uint8_t level;
    // preroll ring: processed audio since start
    int16_t *ring;
    size_t ring_at, ring_n;
    portMUX_TYPE lock;
    // stats (accumulated in us over `frames`)
    yk_audio_stats_t st;
    uint64_t fe_sum, ns_sum, vad_sum, fin_sum;
    // recording for the console
    int16_t *rec;
    size_t rec_cap, rec_n;
    bool rec_raw;
} A = {.lock = portMUX_INITIALIZER_UNLOCKED};

// ---------------------------------------------------------------- models

/** Reads srmodels.bin from the models partition into PSRAM and registers it with ESP-SR. */
static esp_err_t load_models(void) {
    const int64_t t0 = esp_timer_get_time();
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "models");
    if (!p) return ESP_ERR_NOT_FOUND;
    uint8_t *head = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!head) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_partition_read(p, 0, head, 4096);
    const size_t size = err == ESP_OK ? yk_srmodels_size(head, 4096, p->size) : 0;
    heap_caps_free(head);
    if (!size) {
        ESP_LOGW(TAG, "no speech models in the models partition (write srmodels.bin there)");
        return ESP_ERR_NOT_FOUND;
    }
    A.models_buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!A.models_buf) return ESP_ERR_NO_MEM;
    // In 4 KB pieces: reading into PSRAM goes through an internal bounce buffer as big as the piece (up to 16 KB).
    for (size_t off = 0; off < size && err == ESP_OK; off += 4096)
        err = esp_partition_read(p, off, (uint8_t *)A.models_buf + off, size - off < 4096 ? size - off : 4096);
    if (err != ESP_OK) return err;
    srmodel_list_t *models = srmodel_load(A.models_buf);
    char *ns_name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    char *vad_name = esp_srmodel_filter(models, ESP_VADN_PREFIX, NULL);
    if (!ns_name || !vad_name) {
        ESP_LOGW(TAG, "models partition lacks nsnet or vadnet");
        return ESP_ERR_NOT_FOUND;
    }
    // nsnet2's interface directly: calling esp_nsnet_handle_from_name() would link nsnet3 too, 19 KB of internal
    // .bss. Other noise models need a line here.
    A.ns = strncmp(ns_name, "nsnet2", 6) == 0 ? &esp_nsnet2_quantized : NULL;
    A.vad = esp_vadn_handle_from_name(vad_name);
    if (!A.ns || !A.vad) return ESP_ERR_NOT_FOUND;
    // The models' state goes to PSRAM too: malloc() of any size goes there while they are created.
    heap_caps_malloc_extmem_enable(32);
    const size_t before_ns = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    A.ns_data = A.ns->create(ns_name);
    const size_t before_vad = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    // Push-to-talk turns are short: speech counts after 64 ms, silence after 300 ms.
    A.vad_data = A.vad->create(vad_name, VAD_MODE_1, 1, 64, 300);
    const size_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    ESP_LOGI(TAG, "internal RAM: nsnet %d bytes, vadnet %d bytes", (int)(before_ns - before_vad), (int)(before_vad - after));
    if (!A.ns_data || !A.vad_data) return ESP_ERR_NO_MEM;
    A.ns_chunk = A.ns->get_samp_chunksize(A.ns_data);
    A.vad_chunk = A.vad->get_samp_chunksize(A.vad_data);
    A.st.models_bytes = size;
    A.st.models_load_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    A.st.ns_chunk = A.ns_chunk;
    A.st.vad_chunk = A.vad_chunk;
    ESP_LOGI(TAG, "%s + %s: %u bytes in %u ms, chunks %d / %d", ns_name, vad_name, (unsigned)size,
             (unsigned)A.st.models_load_ms, A.ns_chunk, A.vad_chunk);
    return A.ns_chunk > 0 && A.vad_chunk > 0 && A.ns_chunk <= CHUNK_MAX && A.vad_chunk <= CHUNK_MAX ? ESP_OK : ESP_FAIL;
}

// ---------------------------------------------------------------- capture

static void push(fifo_t *f, const int16_t *x, size_t n) {
    if (f->n + n > FIFO_MAX) n = FIFO_MAX - f->n;  // cannot happen with chunks <= 1024; never overrun
    memcpy(f->d + f->n, x, n * sizeof(int16_t));
    f->n += n;
}

static void take(fifo_t *f, int16_t *out, size_t n) {
    memcpy(out, f->d, n * sizeof(int16_t));
    memmove(f->d, f->d + n, (f->n - n) * sizeof(int16_t));
    f->n -= n;
}

static void ring_write(const int16_t *x, size_t n) {
    taskENTER_CRITICAL(&A.lock);
    for (size_t i = 0; i < n; i++) {
        A.ring[A.ring_at] = x[i];
        A.ring_at = (A.ring_at + 1) % PREROLL;
    }
    A.ring_n = A.ring_n + n > PREROLL ? PREROLL : A.ring_n + n;
    taskEXIT_CRITICAL(&A.lock);
}

static void record(const int16_t *x, size_t n) {
    if (!A.rec) return;
    if (A.rec_n + n > A.rec_cap) n = A.rec_cap - A.rec_n;
    memcpy(A.rec + A.rec_n, x, n * sizeof(int16_t));
    A.rec_n += n;
}

/** Gain, limiter and delivery of one chunk (the voice-activity decision is the latest). */
static void deliver(int16_t *pcm, size_t n) {
    const int64_t t0 = esp_timer_get_time();
    for (size_t i = 0; i < n; i++) A.fbuf[i] = pcm[i] / 32768.0f;
    const int speech = A.cfg.voice_activity && A.models ? (A.speech ? 1 : 0) : -1;
    A.level = yk_fe_finish(A.fe, A.fbuf, n, speech, pcm);
    A.fin_sum += (uint64_t)(esp_timer_get_time() - t0);
    ring_write(pcm, n);
    if (!A.rec_raw) record(pcm, n);
    if (A.cfg.on_frame) A.cfg.on_frame(pcm, n, A.speech, A.level, A.cfg.ctx);
}

static void process_frame(void) {
    int64_t t = esp_timer_get_time();
    const size_t k = yk_fe_capture(A.fe, A.tdm, FRAME, A.fbuf);
    int16_t *combined = A.combined;
    for (size_t i = 0; i < k; i++) {
        float v = A.fbuf[i] * 32768.0f;
        combined[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    int64_t now = esp_timer_get_time();
    A.fe_sum += (uint64_t)(now - t);
    if (!A.cfg.noise_suppression || !A.models) {
        if (A.cfg.voice_activity && A.models) {
            push(A.vad_in, combined, k);
            while (A.vad_in->n >= (size_t)A.vad_chunk) {
                take(A.vad_in, A.chunk, A.vad_chunk);
                t = esp_timer_get_time();
                A.speech = A.vad->detect(A.vad_data, A.chunk) == VAD_SPEECH;
                A.vad_sum += (uint64_t)(esp_timer_get_time() - t);
                deliver(A.chunk, A.vad_chunk);
            }
        } else {
            deliver(combined, k);
        }
        return;
    }
    push(A.ns_in, combined, k);
    while (A.ns_in->n >= (size_t)A.ns_chunk) {
        take(A.ns_in, A.ns_buf, A.ns_chunk);
        t = esp_timer_get_time();
        A.ns->process(A.ns_data, A.ns_buf, A.chunk);
        A.ns_sum += (uint64_t)(esp_timer_get_time() - t);
        if (A.cfg.voice_activity) {
            push(A.vad_in, A.chunk, A.ns_chunk);
            while (A.vad_in->n >= (size_t)A.vad_chunk) {
                take(A.vad_in, A.vad_buf, A.vad_chunk);
                t = esp_timer_get_time();
                A.speech = A.vad->detect(A.vad_data, A.vad_buf) == VAD_SPEECH;
                A.vad_sum += (uint64_t)(esp_timer_get_time() - t);
            }
        }
        deliver(A.chunk, A.ns_chunk);
    }
}

static void capture_task(void *arg) {
    (void)arg;
    esp_codec_dev_handle_t mic = audio_mic();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!A.run) continue;
        const int64_t t0 = esp_timer_get_time();
        esp_codec_dev_sample_info_t fs = {
            .sample_rate = I2S_RATE,
            .channel = SLOTS,
            .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) |
                            ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3),
            .bits_per_sample = 16,
        };
        esp_log_level_set("i2s_common", ESP_LOG_NONE);  // "channel not enabled" on reopen is harmless
        const bool open = esp_codec_dev_open(mic, &fs) == ESP_CODEC_DEV_OK;
        esp_log_level_set("i2s_common", ESP_LOG_ERROR);
        if (open) esp_codec_dev_set_in_gain(mic, A.cfg.mic_gain_db > 0 ? A.cfg.mic_gain_db : 30.0f);
        A.st.open_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        A.running = open;
        if (!open) ESP_LOGE(TAG, "microphones did not open");
        int64_t last = esp_timer_get_time();
        while (open && A.run) {
            if (esp_codec_dev_read(mic, (uint8_t *)A.tdm, FRAME * SLOTS * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "read failed");
                break;
            }
            const int64_t t = esp_timer_get_time();
            if (t - last > 40000 && A.st.frames) A.st.late++;
            last = t;
            if (A.rec_raw) record(A.tdm, FRAME * SLOTS);
            process_frame();
            const uint32_t busy = (uint32_t)(esp_timer_get_time() - t);
            if (busy > A.st.busy_max_us) A.st.busy_max_us = busy;
            A.st.frames++;
        }
        if (open) esp_codec_dev_close(mic);
        A.running = false;
        xSemaphoreGive(A.stopped);
    }
}

// ---------------------------------------------------------------- API

esp_err_t yk_audio_init(void) {
    if (A.inited) return ESP_OK;
    const size_t at_start = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    esp_err_t err = audio_init_rate(I2S_RATE);
    ESP_LOGI(TAG, "internal RAM: board audio %u bytes (lowest so far %u)",
             (unsigned)(at_start - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    if (err != ESP_OK) return err;
    if (!audio_mic()) return ESP_ERR_NOT_FOUND;
    A.fe = heap_caps_calloc(1, sizeof(yk_fe_t), MALLOC_CAP_SPIRAM);
    A.tdm = heap_caps_malloc(FRAME * SLOTS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    A.ns_in = heap_caps_calloc(1, sizeof(fifo_t), MALLOC_CAP_SPIRAM);
    A.vad_in = heap_caps_calloc(1, sizeof(fifo_t), MALLOC_CAP_SPIRAM);
    A.ring = heap_caps_malloc(PREROLL * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    A.fbuf = heap_caps_malloc(CHUNK_MAX * sizeof(float), MALLOC_CAP_SPIRAM);
    A.chunk = heap_caps_malloc(CHUNK_MAX * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    A.ns_buf = heap_caps_malloc(CHUNK_MAX * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    A.vad_buf = heap_caps_malloc(CHUNK_MAX * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    A.combined = heap_caps_malloc(FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    A.stopped = xSemaphoreCreateBinary();
    if (!A.fe || !A.tdm || !A.ns_in || !A.vad_in || !A.ring || !A.stopped || !A.fbuf || !A.chunk || !A.ns_buf ||
        !A.vad_buf || !A.combined)
        return ESP_ERR_NO_MEM;
    A.models = load_models() == ESP_OK;
    ESP_LOGI(TAG, "internal RAM: lowest so far %u after the models",
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    if (!A.models) ESP_LOGW(TAG, "running without noise suppression and voice activity models");
    A.st.models = A.models;
    // 5 KB: it uses about 2.1 KB (nsnet2 and vadnet keep their work buffers on the heap, in PSRAM).
    if (xTaskCreatePinnedToCore(capture_task, "yk_capture", 5120, NULL, TASK_PRIO, &A.task, TASK_CORE) != pdPASS)
        return ESP_ERR_NO_MEM;
    A.inited = true;
    ESP_LOGI(TAG, "internal RAM: %u bytes in all, %u free", (unsigned)(at_start - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

esp_err_t yk_audio_start(const yk_audio_config_t *cfg) {
    if (!A.inited) return ESP_ERR_INVALID_STATE;
    if (A.run) return ESP_ERR_INVALID_STATE;
    A.cfg = *cfg;
    yk_fe_config_t fc;
    yk_fe_config_for(cfg->mode, &fc);
    yk_fe_init(A.fe, &YK_MICS_TIKPAL_R1_16K, &fc);
    A.ns_in->n = A.vad_in->n = 0;
    A.speech = false;
    if (A.models) A.vad->clean(A.vad_data);
    taskENTER_CRITICAL(&A.lock);
    A.ring_at = A.ring_n = 0;
    taskEXIT_CRITICAL(&A.lock);
    const bool models = A.st.models;
    const uint32_t bytes = A.st.models_bytes, load = A.st.models_load_ms;
    memset(&A.st, 0, sizeof(A.st));
    A.st.models = models, A.st.models_bytes = bytes, A.st.models_load_ms = load;
    A.st.ns_chunk = A.ns_chunk, A.st.vad_chunk = A.vad_chunk;
    A.fe_sum = A.ns_sum = A.vad_sum = A.fin_sum = 0;
    xSemaphoreTake(A.stopped, 0);
    A.run = true;
    xTaskNotifyGive(A.task);
    return ESP_OK;
}

void yk_audio_stop(void) {
    if (!A.run) return;
    A.run = false;
    xSemaphoreTake(A.stopped, pdMS_TO_TICKS(500));
}

bool yk_audio_running(void) { return A.running; }

size_t yk_audio_preroll(int16_t *out, size_t max_samples) {
    taskENTER_CRITICAL(&A.lock);
    size_t n = A.ring_n < max_samples ? A.ring_n : max_samples;
    size_t from = (A.ring_at + PREROLL - n) % PREROLL;
    for (size_t i = 0; i < n; i++) out[i] = A.ring[(from + i) % PREROLL];
    taskEXIT_CRITICAL(&A.lock);
    return n;
}

uint8_t yk_audio_level(void) { return A.level; }

// ---------------------------------------------------------------- push-to-talk recording (P2)

static struct {
    yk_ptt_t buf;
    uint8_t *mem;
    volatile bool on;
} R;

/** From the capture task: keeps the frame; full, it ends the capture itself (yk_audio_stop() would wait for this
 *  very task). */
static void rec_frame(const int16_t *pcm, size_t n, bool speech, uint8_t level, void *ctx) {
    (void)level, (void)ctx;
    if (!R.on) return;
    if (!yk_ptt_add(&R.buf, pcm, n, speech)) {
        R.on = false;
        A.run = false;
    }
}

esp_err_t yk_audio_rec_init(int max_seconds) {
    if (max_seconds < 1 || max_seconds > 60) return ESP_ERR_INVALID_ARG;
    esp_err_t err = yk_audio_init();
    if (err != ESP_OK) return err;
    const size_t bytes = yk_ptt_bytes(YK_AUDIO_RATE, max_seconds);
    if (R.mem && R.buf.cap * 2 + YK_PTT_HEADER >= bytes) return ESP_OK;
    heap_caps_free(R.mem);
    R.mem = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!R.mem) return ESP_ERR_NO_MEM;
    yk_ptt_init(&R.buf, R.mem, bytes, YK_AUDIO_RATE);
    return ESP_OK;
}

esp_err_t yk_audio_rec_start(void) {
    if (!R.mem || A.run) return ESP_ERR_INVALID_STATE;
    yk_ptt_reset(&R.buf);
    R.on = true;
    const yk_audio_config_t cfg = {.mode = YK_AUDIO_PUSH_TO_TALK, .noise_suppression = true, .voice_activity = true,
                                   .on_frame = rec_frame};
    const esp_err_t err = yk_audio_start(&cfg);
    if (err != ESP_OK) R.on = false;
    return err;
}

void yk_audio_rec_stop(void) {
    R.on = false;
    yk_audio_stop();
}

bool yk_audio_rec_running(void) { return R.on && A.run; }

const uint8_t *yk_audio_rec_wav(size_t *len, float *seconds, bool *speech) {
    if (R.on) yk_audio_rec_stop();
    const size_t n = R.mem ? yk_ptt_finish(&R.buf) : 0;
    if (len) *len = n;
    if (seconds) *seconds = R.mem ? (float)R.buf.n / YK_AUDIO_RATE : 0;
    if (speech) *speech = R.mem && R.buf.speech_frames > 0;
    return n ? R.mem : NULL;
}

void yk_audio_stats(yk_audio_stats_t *out) {
    *out = A.st;
    const uint32_t f = A.st.frames ? A.st.frames : 1;
    out->fe_us = (uint32_t)(A.fe_sum / f);
    out->ns_us = (uint32_t)(A.ns_sum / f);
    out->vad_us = (uint32_t)(A.vad_sum / f);
    out->finish_us = (uint32_t)(A.fin_sum / f);
}

// ---------------------------------------------------------------- console

#include "esp_console.h"
#include "mbedtls/base64.h"

void yk_audio_print_wav(const int16_t *pcm, size_t samples, int channels, uint32_t rate) {
    const uint32_t data = (uint32_t)(samples * sizeof(int16_t));
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0};
    h[22] = (uint8_t)channels;
    const uint32_t riff = 36 + data, byte_rate = rate * channels * 2, block = channels * 2;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &block, 2);
    h[34] = 16;
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data, 4);
    printf("AUDIO_WAV_BEGIN %u %u %d\n", (unsigned)(44 + data), (unsigned)rate, channels);
    unsigned char line[96];
    size_t len;
    // Each line is "AW <seq> <base64>": a log line from another task can land between them, and the reader keeps
    // only well-formed lines in sequence (an unframed dump shifted the TDM slots wherever a log line got decoded).
    mbedtls_base64_encode(line, sizeof(line), &len, h, 44);
    line[len] = 0;
    printf("AW 0 %s\n", (char *)line);
    const uint8_t *bytes = (const uint8_t *)pcm;
    for (size_t off = 0; off < data; off += 57) {
        const size_t n = data - off < 57 ? data - off : 57;
        mbedtls_base64_encode(line, sizeof(line), &len, bytes + off, n);
        line[len] = 0;
        printf("AW %u %s\n", (unsigned)(off / 57 + 1), (char *)line);
        if ((off / 57) % 128 == 127) vTaskDelay(1);
    }
    puts("AUDIO_WAV_END");
}

static void print_stats(const char *tag) {
    yk_audio_stats_t s;
    yk_audio_stats(&s);
    printf("AUDIO_STAT %s models=%d model_bytes=%u model_load_ms=%u ns_chunk=%u vad_chunk=%u frames=%u late=%u "
           "open_ms=%u fe_us=%u ns_us=%u vad_us=%u finish_us=%u busy_max_us=%u cpu_pct=%.1f\n",
           tag, s.models, (unsigned)s.models_bytes, (unsigned)s.models_load_ms, (unsigned)s.ns_chunk,
           (unsigned)s.vad_chunk, (unsigned)s.frames, (unsigned)s.late, (unsigned)s.open_ms, (unsigned)s.fe_us,
           (unsigned)s.ns_us, (unsigned)s.vad_us, (unsigned)s.finish_us, (unsigned)s.busy_max_us,
           (s.fe_us + s.ns_us + s.vad_us + s.finish_us) / 200.0);  // per 20 ms of audio
}

#define OPUS_TEST_STACK (32 * 1024)

typedef struct {
    int complexity, bitrate;
    uint32_t packets, bytes, max_us, stack_used;
    uint64_t sum_us;
    SemaphoreHandle_t done;
} opus_job_t;

static void opus_task(void *arg) {
    opus_job_t *job = arg;
    yk_opus_enc_t *enc = yk_opus_enc_create(job->complexity, job->bitrate);
    for (size_t off = 0; enc && off + 960 <= A.rec_n; off += 960) {
        const int got = yk_opus_enc_feed(enc, A.rec + off, 960, NULL, NULL);
        if (got < 0) break;
        const uint32_t us = yk_opus_enc_last_us(enc);
        job->packets += (uint32_t)got;
        job->sum_us += us;
        if (us > job->max_us) job->max_us = us;
    }
    if (enc) job->bytes = (uint32_t)yk_opus_enc_bytes(enc);
    yk_opus_enc_destroy(enc);
    job->stack_used = OPUS_TEST_STACK - uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
    xSemaphoreGive(job->done);
    vTaskSuspend(NULL);  // deleted by the console with its PSRAM stack
}

static int audio_cmd(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "stat";
    if (!strcmp(sub, "stat")) {
        print_stats("now");
        return 0;
    }
    if (!strcmp(sub, "rec") && argc > 2) {
        // audio rec <ms> [raw|plain|nsonly]: raw = the 4 TDM slots; plain = no models; nsonly = no voice model
        const int ms = atoi(argv[2]);
        const char *how = argc > 3 ? argv[3] : "";
        if (ms <= 0 || ms > 10000 || A.run) return 1;
        A.rec_raw = !strcmp(how, "raw");
        A.rec_cap = (size_t)(A.rec_raw ? I2S_RATE : YK_AUDIO_RATE) * ms / 1000 * (A.rec_raw ? SLOTS : 1);
        A.rec = heap_caps_malloc(A.rec_cap * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!A.rec) return 1;
        A.rec_n = 0;
        const yk_audio_config_t cfg = {.mode = YK_AUDIO_PUSH_TO_TALK,
                                       .noise_suppression = strcmp(how, "plain") != 0,
                                       .voice_activity = strcmp(how, "plain") != 0 && strcmp(how, "nsonly") != 0};
        yk_audio_start(&cfg);
        vTaskDelay(pdMS_TO_TICKS(ms + 100));
        yk_audio_stop();
        print_stats(how[0] ? how : "rec");
        yk_audio_print_wav(A.rec, A.rec_n, A.rec_raw ? SLOTS : 1, A.rec_raw ? I2S_RATE : YK_AUDIO_RATE);
        heap_caps_free(A.rec);
        A.rec = NULL;
        return 0;
    }
    if (!strcmp(sub, "opus") && argc > 2) {
        // audio opus <complexity> [bitrate]: 3 s of capture, then encoded in a task of its own (libopus needs a
        // deep stack); time per 60 ms packet, size, and the stack the encoder used.
        static opus_job_t job;
        job = (opus_job_t){.complexity = atoi(argv[2]), .bitrate = argc > 3 ? atoi(argv[3]) : 16000};
        const uint32_t stack_caps = argc > 4 && !strcmp(argv[4], "internal") ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM;
        A.rec_raw = false;
        A.rec_cap = YK_AUDIO_RATE * 3;
        A.rec = heap_caps_malloc(A.rec_cap * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!A.rec) return 1;
        A.rec_n = 0;
        const yk_audio_config_t cfg = {.mode = YK_AUDIO_PUSH_TO_TALK, .noise_suppression = true, .voice_activity = true};
        yk_audio_start(&cfg);
        vTaskDelay(pdMS_TO_TICKS(3100));
        yk_audio_stop();
        job.done = xSemaphoreCreateBinary();
        TaskHandle_t t;
        if (xTaskCreatePinnedToCoreWithCaps(opus_task, "yk_opus_test", OPUS_TEST_STACK, &job, 5, &t, 0, stack_caps) !=
            pdPASS) {
            heap_caps_free(A.rec);
            A.rec = NULL;
            return 1;
        }
        xSemaphoreTake(job.done, portMAX_DELAY);
        vTaskDeleteWithCaps(t);
        vSemaphoreDelete(job.done);
        printf("AUDIO_OPUS stack=%s complexity=%d packets=%u bytes=%u kbps=%.1f us_avg=%u us_max=%u cpu_pct=%.1f stack_used=%u\n",
               stack_caps == MALLOC_CAP_INTERNAL ? "internal" : "psram", job.complexity, (unsigned)job.packets,
               (unsigned)job.bytes,
               job.packets ? job.bytes * 8.0 / (job.packets * 60.0) : 0.0,
               job.packets ? (unsigned)(job.sum_us / job.packets) : 0, (unsigned)job.max_us,
               job.packets ? job.sum_us / (double)job.packets / 600.0 : 0.0, (unsigned)job.stack_used);
        heap_caps_free(A.rec);
        A.rec = NULL;
        return 0;
    }
    printf("audio stat | rec <ms> [raw|plain|nsonly] | opus <complexity> [bitrate]\n");
    return 1;
}

esp_err_t yk_audio_register_console(void) {
    const esp_console_cmd_t cmd = {.command = "audio", .help = "audio stat | rec <ms> [raw|plain|nsonly] | opus <c>",
                                   .func = audio_cmd};
    return esp_console_cmd_register(&cmd);
}

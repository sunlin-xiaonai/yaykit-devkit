#include "yaykit_speak.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "board.h"
#include "decoder/impl/esp_opus_dec.h"
#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "yk_mix.h"

// The speaker task writes one block at a time (20 ms, or 5 ms for low latency), at most `ahead` blocks before they
// are heard. A "stretch" is a run of blocks while something plays (an utterance, effects, or a held amplifier): the
// speaker's clock counts samples from its first block. Between stretches the I2S DMA plays zeros (auto_clear).
#define MAX_BLOCK   YK_MIX_MAX_BLOCK  // 20 ms at 24 kHz
#define MAX_PACKET  1276
#define PACKETS     8192   // bytes of packets waiting to be decoded (~4 s at 16 kbit/s)
#define STACK       (16 * 1024)  // libopus decodes on the stack (about 8 KB used); PSRAM (no flash writes here)
#define IDLE_CLOSE_MS 5000
#define LEVELS      64     // per block, by index % LEVELS: enough for the blocks ahead at 5 ms
// Measured on the Tikpal R1: the amplifier is mute for about 300 ms after GPIO43 goes high, then at full level
// at once. Sound in that time is lost, so playback waits it out with silence; yk_speak_prepare() starts it early.
#define PA_WARM_MS  YK_SPEAK_PA_WARM_MS
#define PA_LINGER_MS 3000  // on after the last sound, so the next sentence or cue need not wait again

static const char *TAG = "yk_speak";

enum { IDLE, BUFFERING, PLAYING, DRAINING };

static struct {
    TaskHandle_t task;
    MessageBufferHandle_t packets;
    SemaphoreHandle_t packets_lock;  // send, receive and reset come from different tasks; none of them blocks
    SemaphoreHandle_t mix_lock;      // the mixer: the task per block, the yk_sfx_* calls (recursive: from callbacks)
    portMUX_TYPE lock;
    uint32_t rate, block, block_us, ahead;
    uint32_t lead;        // blocks written ahead now: `ahead`, or 1 while an utterance only buffers (no silence
                          // queued in front of its first sound)
    uint32_t ring_cap, start_at, fade, decode_ahead, max_pcm;
    yk_ring_t ring;
    void *dec;
    esp_codec_dev_handle_t spk;
    bool open;
    volatile int state;
    volatile bool ended, stop, live, hold_amp;
    volatile bool reclaim;  // yk_speak_reclaim(): open and close the codec once, when idle
    volatile bool mic;      // yk_speak_mic(): the microphones are recording
    SemaphoreHandle_t reclaim_lock;  // a reclaim's check and its codec open/close, against yk_speak_mic(true)
    int volume;
    yk_mix_t *mix;
    int16_t *pcm;     // max_pcm: one decoded packet
    int16_t *chunk;   // the utterance's block
    int16_t *mono;    // the mixed block
    int16_t *out;     // stereo
    uint8_t *packet;  // MAX_PACKET
    int16_t *tone;    // the chime
    const uint8_t *clip;  // an Opus clip being played as the utterance (yk_speak_clip)
    size_t clip_bytes, clip_pos;
    // the stretch
    bool running;
    int64_t t_play;       // us: the first block handed to I2S
    uint32_t written;     // blocks handed to I2S since t_play
    uint32_t drain_at;    // DRAINING: idle once this many blocks are heard
    uint8_t levels[LEVELS];
    int64_t t_begin;
    bool faded_in, started;
    int64_t out_delay_us;  // from a write to the speaker: about one DMA frame (the one playing)
    uint64_t decode_total, mix_total;
    uint32_t mix_blocks;
    yk_speak_stats_t st;
    volatile bool pa_on;
    volatile int64_t pa_on_us, pa_off_us;  // when the amplifier came on; when it may go off (0 = not before use)
} S = {.lock = portMUX_INITIALIZER_UNLOCKED, .volume = 6};

static int64_t now_us(void) { return esp_timer_get_time(); }

static size_t ring_avail(void) {
    taskENTER_CRITICAL(&S.lock);
    const size_t n = yk_ring_avail(&S.ring);
    taskEXIT_CRITICAL(&S.lock);
    return n;
}

static size_t ring_space(void) { return S.ring_cap - ring_avail(); }

static size_t ring_write(const int16_t *x, size_t n) {
    taskENTER_CRITICAL(&S.lock);
    n = yk_ring_write(&S.ring, x, n);
    taskEXIT_CRITICAL(&S.lock);
    return n;
}

static size_t ring_read(int16_t *x, size_t n) {
    taskENTER_CRITICAL(&S.lock);
    n = yk_ring_read(&S.ring, x, n);
    taskEXIT_CRITICAL(&S.lock);
    return n;
}

static void ring_clear(void) {
    taskENTER_CRITICAL(&S.lock);
    yk_ring_clear(&S.ring);
    taskEXIT_CRITICAL(&S.lock);
}

static void pa(bool on) {
    taskENTER_CRITICAL(&S.lock);
    const bool was = S.pa_on;
    S.pa_on = on;
    if (on && !was) S.pa_on_us = now_us();
    S.pa_off_us = 0;
    taskEXIT_CRITICAL(&S.lock);
    if (on != was) gpio_set_level(BOARD_PIN_PA_EN, on ? 1 : 0);
}

/** The amplifier stays on for PA_LINGER_MS more, then goes off unless something plays. */
static void pa_linger(void) {
    taskENTER_CRITICAL(&S.lock);
    if (S.pa_on && !S.hold_amp) S.pa_off_us = now_us() + PA_LINGER_MS * 1000LL;
    taskEXIT_CRITICAL(&S.lock);
}

/** When the amplifier is (or will be) up, esp_timer us. */
static int64_t warm_at(void) { return S.pa_on ? S.pa_on_us + PA_WARM_MS * 1000LL : INT64_MAX; }

static bool codec_open(void) {
    if (S.open) return true;
    const int64_t t0 = now_us();
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = S.rate, .channel = 2, .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(S.spk, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "speaker open failed");
        return false;
    }
    esp_codec_dev_set_out_vol(S.spk, yk_volume_to_codec(S.volume));
    S.open = true;
    S.st.open_ms = (uint32_t)((now_us() - t0) / 1000);
    return true;
}

static void codec_close(void) {
    if (!S.open) return;
    esp_codec_dev_close(S.spk);
    S.open = false;
}

/** Blocks the I2S buffer has played since t_play (both run off the crystal). */
static uint32_t blocks_heard(void) {
    if (!S.t_play) return 0;
    return (uint32_t)((now_us() - S.t_play) / S.block_us);
}

/** When the next block to be written will be heard. */
static int64_t next_heard_us(void) {
    if (!S.t_play) return now_us() + S.out_delay_us;
    return S.t_play + (int64_t)S.written * S.block_us + S.out_delay_us;
}

/** Hands one mono block to I2S, at most `ahead` blocks before it is heard. */
static void write_block(const int16_t *mono) {
    if (S.t_play) {
        while (S.written > blocks_heard() + S.lead) vTaskDelay(1);
    }
    for (uint32_t i = 0; i < S.block; i++) S.out[2 * i] = S.out[2 * i + 1] = mono[i];
    if (!S.t_play) S.t_play = now_us();
    S.written++;
    esp_codec_dev_write(S.spk, S.out, S.block * 2 * sizeof(int16_t));
}

/** The next packet of the clip being played, or 0 when it is done. */
static size_t clip_packet(uint8_t *out) {
    if (!S.clip || S.clip_pos + 2 > S.clip_bytes) return 0;
    const size_t len = S.clip[S.clip_pos] | (S.clip[S.clip_pos + 1] << 8);
    if (!len || len > MAX_PACKET || S.clip_pos + 2 + len > S.clip_bytes) {
        S.clip_pos = S.clip_bytes;  // a broken clip ends here
        return 0;
    }
    memcpy(out, S.clip + S.clip_pos + 2, len);
    S.clip_pos += 2 + len;
    return len;
}

static bool clip_done(void) { return !S.clip || S.clip_pos >= S.clip_bytes; }

/** Decodes waiting packets until decode_ahead is buffered (while the ring has room for one more). */
static void decode_some(void) {
    while (ring_avail() < S.decode_ahead && ring_space() >= S.max_pcm) {
        size_t len = 0;
        if (S.clip) {
            len = clip_packet(S.packet);
        } else if (xMessageBufferIsEmpty(S.packets) == pdFALSE) {
            xSemaphoreTake(S.packets_lock, portMAX_DELAY);
            len = xMessageBufferReceive(S.packets, S.packet, MAX_PACKET, 0);
            xSemaphoreGive(S.packets_lock);
        }
        if (!len) break;
        esp_audio_dec_in_raw_t raw = {.buffer = S.packet, .len = len};
        esp_audio_dec_out_frame_t frame = {.buffer = (uint8_t *)S.pcm, .len = S.max_pcm * sizeof(int16_t)};
        esp_audio_dec_info_t info;
        const int64_t t0 = now_us();
        const esp_audio_err_t err = esp_opus_dec_decode(S.dec, &raw, &frame, &info);
        const uint32_t us = (uint32_t)(now_us() - t0);
        S.st.packets++;
        S.decode_total += us;
        if (us > S.st.decode_max_us) S.st.decode_max_us = us;
        if (err != ESP_AUDIO_ERR_OK) {
            S.st.decode_errors++;
            continue;
        }
        ring_write(S.pcm, frame.decoded_size / sizeof(int16_t));
    }
}

static void reset_packets(void) {
    xSemaphoreTake(S.packets_lock, portMAX_DELAY);
    xMessageBufferReset(S.packets);
    xSemaphoreGive(S.packets_lock);
}

static bool all_in(void) { return S.ended && xMessageBufferIsEmpty(S.packets) == pdTRUE && clip_done(); }

static int sfx_active(void) {
    xSemaphoreTakeRecursive(S.mix_lock, portMAX_DELAY);
    const int n = yk_mix_active(S.mix);
    xSemaphoreGiveRecursive(S.mix_lock);
    return n;
}

/** Something needs the speaker: an utterance, an effect, or the amplifier held for a game. */
static bool wanted(void) { return S.state != IDLE || sfx_active() || S.hold_amp; }

/** The stretch is over: let the I2S buffer play out, the amplifier lingers, the clock stops. */
static void end_stretch(void) {
    while (S.t_play && blocks_heard() <= S.written) vTaskDelay(pdMS_TO_TICKS(2));
    vTaskDelay(pdMS_TO_TICKS(20));
    pa_linger();
    S.st.played_ms += S.written * S.block_us / 1000;
    S.t_play = 0;
    S.written = 0;
    S.running = false;
}

static void start_stretch(void) {
    if (!codec_open()) {
        // nothing can be heard: drop what waits
        ring_clear();
        reset_packets();
        S.clip = NULL;
        S.state = IDLE;
        xSemaphoreTakeRecursive(S.mix_lock, portMAX_DELAY);
        yk_mix_init(S.mix, S.rate);
        xSemaphoreGiveRecursive(S.mix_lock);
        S.hold_amp = false;
        vTaskDelay(pdMS_TO_TICKS(100));  // not again at once
        return;
    }
    pa(true);
    S.t_play = 0;
    S.written = 0;
    S.lead = 1;
    S.running = true;
}

/** One block of the utterance into S.chunk; returns it, or NULL when none sounds in this block. */
static const int16_t *utterance_block(int64_t heard_us) {
    if (S.stop) {
        const int was = S.state;
        S.stop = false;
        const int16_t *r = NULL;
        if (was == PLAYING) {
            // Fade out from where it is: the blocks already in the I2S buffer play, then this one fades.
            const size_t n = ring_read(S.chunk, S.block);
            memset(S.chunk + n, 0, (S.block - n) * sizeof(int16_t));
            const size_t f = S.fade < S.block ? S.fade : S.block;
            yk_fade(S.chunk, f, 1, 0);
            memset(S.chunk + f, 0, (S.block - f) * sizeof(int16_t));
            r = S.chunk;
        }
        ring_clear();
        reset_packets();
        S.clip = NULL;
        if (was == PLAYING || was == DRAINING) {
            S.state = DRAINING;
            S.drain_at = S.written + 1;
        } else {
            S.state = IDLE;
        }
        return r;
    }
    if (S.state == IDLE || S.state == DRAINING) return NULL;
    decode_some();
    if (S.state == BUFFERING) {
        const size_t avail = ring_avail();
        if (all_in() && !avail) {
            S.state = IDLE;  // an empty utterance
            S.clip = NULL;
            return NULL;
        }
        if (!(avail >= S.start_at || (all_in() && avail))) return NULL;
        if (heard_us < warm_at()) return NULL;  // the amplifier is not up yet: silence until it is
        if (!S.started) {
            S.st.first_ms = (uint32_t)((now_us() - S.t_begin) / 1000);
            const int64_t w = warm_at() - S.t_begin;
            S.st.warm_ms = w > 0 && w < 10000000 ? (uint32_t)(w / 1000) : 0;
        }
        S.started = true;
        S.faded_in = false;
        S.state = PLAYING;
    }
    size_t n = ring_read(S.chunk, S.block);
    if (n < S.block) {
        if (all_in()) {
            if (n) {
                const size_t f = n > S.fade ? S.fade : n;
                yk_fade(S.chunk + n - f, f, 1, 0);
            }
            memset(S.chunk + n, 0, (S.block - n) * sizeof(int16_t));
            S.state = DRAINING;  // idle once this block is heard (yk_speak_wait returns then)
            S.drain_at = S.written + 1;
            S.clip = NULL;
            return S.chunk;
        }
        S.st.underruns++;
        if (!n) {
            // The network is behind: wait for 120 ms again rather than stutter.
            S.state = BUFFERING;
            return NULL;
        }
        memset(S.chunk + n, 0, (S.block - n) * sizeof(int16_t));
    }
    if (!S.faded_in) {
        yk_fade(S.chunk, S.fade < S.block ? S.fade : S.block, 0, 1);
        S.faded_in = true;
    }
    return S.chunk;
}

static void speak_task(void *arg) {
    (void)arg;
    for (;;) {
        if (!S.running) {
            if (wanted()) {
                start_stretch();
                continue;
            }
            // Wake for the next sound, or to switch the amplifier off, or to close the codec when unused.
            const int64_t off = S.pa_off_us;
            TickType_t wait = S.open ? pdMS_TO_TICKS(IDLE_CLOSE_MS) : portMAX_DELAY;
            if (off) {
                const int64_t ms = (off - now_us()) / 1000;
                wait = ms > 0 ? pdMS_TO_TICKS(ms) + 1 : 0;
            }
            const bool woken = ulTaskNotifyTake(pdTRUE, wait);
            if (wanted()) continue;
            if (S.reclaim) {
                // The microphones left the shared I2S in their format: the speaker's again (amplifier off: silent).
                // Not while they record again (checked and done under the lock yk_speak_mic takes).
                S.reclaim = false;
                xSemaphoreTake(S.reclaim_lock, portMAX_DELAY);
                if (yk_speak_may_reclaim(S.open, wanted(), S.mic) && codec_open()) codec_close();
                xSemaphoreGive(S.reclaim_lock);
                continue;
            }
            if (S.pa_off_us && now_us() >= S.pa_off_us) pa(false);
            else if (!woken && !S.pa_on && S.open) codec_close();
            continue;
        }
        if (S.state == DRAINING && blocks_heard() >= S.drain_at) S.state = IDLE;
        if (!wanted() && S.state == IDLE) {
            end_stretch();
            continue;
        }
        if (S.t_play && blocks_heard() > S.written) {
            // The speaker ran dry (the task was held up): it played zeros. Start the clock again from now.
            S.st.late_blocks++;
            S.t_play = now_us() - (int64_t)S.written * S.block_us;
        }
        const int64_t heard = next_heard_us();
        const uint64_t sample0 = (uint64_t)S.written * S.block;
        const int16_t *utter = utterance_block(heard);
        const int64_t t0 = now_us();
        xSemaphoreTakeRecursive(S.mix_lock, portMAX_DELAY);
        if (S.live && yk_mix_active(S.mix)) yk_mix_stop(S.mix, 0, S.rate / 100);
        yk_mix_render(S.mix, utter, S.block, sample0, heard, S.mono);
        const int active = yk_mix_active(S.mix);
        xSemaphoreGiveRecursive(S.mix_lock);
        const uint32_t us = (uint32_t)(now_us() - t0);
        S.mix_total += us;
        S.mix_blocks++;
        if (us > S.st.mix_max_us) S.st.mix_max_us = us;
        if ((uint32_t)active > S.st.sfx_max) S.st.sfx_max = (uint32_t)active;
        S.lead = S.state == BUFFERING && !active && !S.hold_amp ? 1 : S.ahead;
        S.levels[S.written % LEVELS] = utter ? yk_level16(utter, S.block) : 0;
        write_block(S.mono);
    }
}

esp_err_t yk_speak_init(void) { return yk_speak_init_cfg(NULL); }

esp_err_t yk_speak_init_cfg(const yk_speak_config_t *cfg) {
    if (S.task) return ESP_OK;
    const yk_speak_config_t none = {0};
    if (!cfg) cfg = &none;
    const uint32_t block_ms = cfg->block_ms == 5 ? 5 : 20;
    uint32_t rate = cfg->rate ? cfg->rate : (audio_sample_rate() ? audio_sample_rate() : YK_SPEAK_RATE);
    if (rate != 16000 && rate != 24000) return ESP_ERR_INVALID_ARG;
    if (!audio_sample_rate()) {
        const audio_config_t ac = {.sample_rate = rate, .frame_ms = (uint8_t)block_ms,
                                   .frames = cfg->dma_frames ? cfg->dma_frames : (block_ms == 5 ? 4 : 8)};
        esp_err_t err = audio_init_cfg(&ac);
        if (err != ESP_OK) return err;
    }
    if (audio_sample_rate() != rate) {
        ESP_LOGE(TAG, "the board's audio runs at %u Hz, not %u", (unsigned)audio_sample_rate(), (unsigned)rate);
        return ESP_ERR_INVALID_STATE;
    }
    S.spk = audio_speaker();
    if (!S.spk) return ESP_ERR_NOT_FOUND;
    S.rate = rate;
    S.block = rate * block_ms / 1000;
    S.block_us = block_ms * 1000;
    S.ahead = cfg->ahead ? cfg->ahead : 4;
    S.ring_cap = rate == 24000 ? 65536 : 32768;  // samples, a power of two: 2.7 / 2.0 s
    S.start_at = rate * 120 / 1000;
    S.fade = rate / 100;
    S.decode_ahead = rate * 300 / 1000;  // decode only this far ahead, so decoding never takes more than ~8 ms at
                                         // once and the capture task on the same core is not held up
    S.max_pcm = rate * 120 / 1000;       // one Opus packet decodes to at most 120 ms
    const uint32_t dma_frame = audio_dma_frame_samples();
    S.out_delay_us = dma_frame ? (int64_t)dma_frame * 1000000 / rate : 20000;
    int16_t *ring = heap_caps_malloc(S.ring_cap * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    S.pcm = heap_caps_malloc(S.max_pcm * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    S.chunk = heap_caps_malloc(MAX_BLOCK * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    S.mono = heap_caps_malloc(MAX_BLOCK * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    S.out = heap_caps_malloc(MAX_BLOCK * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    S.packet = heap_caps_malloc(MAX_PACKET, MALLOC_CAP_SPIRAM);
    S.tone = heap_caps_malloc(rate / 5 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    S.mix = heap_caps_calloc(1, sizeof(yk_mix_t), MALLOC_CAP_SPIRAM);
    S.packets = xMessageBufferCreateWithCaps(PACKETS, MALLOC_CAP_SPIRAM);
    S.packets_lock = xSemaphoreCreateMutex();
    S.mix_lock = xSemaphoreCreateRecursiveMutex();
    S.reclaim_lock = xSemaphoreCreateMutex();
    if (!S.packets_lock || !S.mix_lock || !S.reclaim_lock) return ESP_ERR_NO_MEM;
    if (!ring || !S.pcm || !S.chunk || !S.mono || !S.out || !S.packet || !S.tone || !S.mix || !S.packets)
        return ESP_ERR_NO_MEM;
    yk_ring_init(&S.ring, ring, S.ring_cap);
    yk_mix_init(S.mix, rate);
    esp_opus_dec_cfg_t oc = ESP_OPUS_DEC_CONFIG_DEFAULT();
    oc.sample_rate = rate;  // libopus decodes any stream straight to this rate
    oc.channel = 1;
    if (esp_opus_dec_open(&oc, sizeof(oc), &S.dec) != ESP_AUDIO_ERR_OK) return ESP_FAIL;
    S.volume = board_pref_volume();  // the device's shared volume (P10)
    S.st.rate = rate;
    S.st.block_ms = block_ms;
    gpio_reset_pin(BOARD_PIN_PA_EN);
    gpio_set_direction(BOARD_PIN_PA_EN, GPIO_MODE_OUTPUT);
    pa(false);
    if (xTaskCreatePinnedToCoreWithCaps(speak_task, "yk_speak", STACK, NULL, 8, &S.task, 1, MALLOC_CAP_SPIRAM) !=
        pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "ready: %u Hz, %u ms blocks, %u ahead, DMA %u x %u, volume %d", (unsigned)rate, (unsigned)block_ms,
             (unsigned)S.ahead, (unsigned)audio_dma_frames(), (unsigned)dma_frame, S.volume);
    return ESP_OK;
}

uint32_t yk_speak_rate(void) { return S.task ? S.rate : 0; }

void yk_speak_reclaim(void) {
    if (!S.task) return;
    S.reclaim = true;
    xTaskNotifyGive(S.task);
}

void yk_speak_mic(bool recording) {
    if (!S.reclaim_lock) {
        S.mic = recording;
        return;
    }
    xSemaphoreTake(S.reclaim_lock, portMAX_DELAY);  // a reclaim going on finishes first (about 20 ms)
    S.mic = recording;
    xSemaphoreGive(S.reclaim_lock);
}

void yk_speak_cancel(void) {
    if (!S.task || S.state == IDLE || S.state == DRAINING) return;
    S.stop = true;
    xTaskNotifyGive(S.task);
}

void yk_speak_stop(void) {
    if (!S.task) return;
    if (S.state != IDLE && S.state != DRAINING) {
        S.stop = true;
        xTaskNotifyGive(S.task);
    }
    for (int i = 0; i < 60 && S.state != IDLE; i++) vTaskDelay(pdMS_TO_TICKS(5));
}

static esp_err_t begin(const uint8_t *clip, size_t clip_bytes) {
    if (!S.task) return ESP_ERR_INVALID_STATE;
    yk_speak_stop();
    ring_clear();
    reset_packets();
    esp_opus_dec_reset(S.dec);
    S.clip_pos = 0;
    S.clip_bytes = clip_bytes;
    S.clip = clip;
    S.ended = clip != NULL;  // a clip brings all its packets itself
    S.started = false;
    S.stop = false;
    S.t_begin = now_us();
    S.st.utterances++;
    S.state = BUFFERING;
    xTaskNotifyGive(S.task);
    return ESP_OK;
}

esp_err_t yk_speak_begin(void) { return begin(NULL, 0); }

esp_err_t yk_speak_clip(const yk_clip_t *clip) {
    if (!clip || clip->format != YK_CLIP_OPUS || !clip->data || clip->bytes < 3) return ESP_ERR_INVALID_ARG;
    return begin(clip->data, clip->bytes);
}

esp_err_t yk_speak_opus_wait(const uint8_t *packet, size_t bytes, int timeout_ms) {
    if (!S.task || S.state == IDLE || S.clip) return ESP_ERR_INVALID_STATE;
    if (!packet || !bytes || bytes > MAX_PACKET) return ESP_ERR_INVALID_ARG;
    // Never a blocked send: begin() and stop() reset the buffer, which FreeRTOS forbids with a sender waiting.
    const int64_t give_up = now_us() + (int64_t)timeout_ms * 1000;
    for (;;) {
        xSemaphoreTake(S.packets_lock, portMAX_DELAY);
        const size_t sent = xMessageBufferSpacesAvailable(S.packets) >= bytes + sizeof(size_t)
                                ? xMessageBufferSend(S.packets, packet, bytes, 0)
                                : 0;
        xSemaphoreGive(S.packets_lock);
        if (sent == bytes) {
            xTaskNotifyGive(S.task);
            return ESP_OK;
        }
        if (now_us() >= give_up || S.state == IDLE) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t yk_speak_opus(const uint8_t *packet, size_t bytes) { return yk_speak_opus_wait(packet, bytes, 3000); }

esp_err_t yk_speak_pcm(const int16_t *pcm, size_t samples) {
    if (!S.task || S.state == IDLE || S.clip) return ESP_ERR_INVALID_STATE;
    const int64_t give_up = now_us() + 3000000;
    while (samples) {
        const size_t n = ring_write(pcm, samples);
        pcm += n, samples -= n;
        xTaskNotifyGive(S.task);
        if (samples) {
            if (now_us() > give_up || S.state == IDLE) return ESP_ERR_TIMEOUT;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    return ESP_OK;
}

void yk_speak_end(void) {
    if (!S.task) return;
    S.ended = true;
    xTaskNotifyGive(S.task);
}

bool yk_speak_busy(void) { return S.state != IDLE; }

void yk_speak_prepare(void) {
    if (!S.task) return;
    pa(true);
    pa_linger();
    xTaskNotifyGive(S.task);  // re-arm the switch-off time
}

void yk_speak_hold_amp(bool on) {
    if (!S.task) return;
    S.hold_amp = on;
    if (on) pa(true);
    else pa_linger();
    xTaskNotifyGive(S.task);
}

void yk_speak_set_live(bool on) {
    S.live = on;
    if (on && S.task) xTaskNotifyGive(S.task);
}

bool yk_speak_wait(int timeout_ms) {
    const int64_t end = now_us() + (int64_t)timeout_ms * 1000;
    while (S.state != IDLE) {
        if (now_us() > end) return false;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return true;
}

uint8_t yk_speak_level(void) {
    if ((S.state != PLAYING && S.state != DRAINING) || !S.t_play) return 0;
    const uint32_t heard = blocks_heard();
    return heard < S.written && heard + LEVELS > S.written ? S.levels[heard % LEVELS] : 0;
}

void yk_speak_clock(yk_speak_clock_t *out) {
    *out = (yk_speak_clock_t){0};
    if (!S.task || !S.running || !S.t_play) return;
    const int64_t now = now_us(), since = now - S.t_play - S.out_delay_us;
    out->running = true;
    out->at_us = now;
    out->sample = since > 0 ? (uint64_t)since * S.rate / 1000000 : 0;
}

int yk_speak_latency_ms(void) {
    if (!S.task) return 0;
    int ms = (int)((S.ahead * S.block_us + S.out_delay_us) / 1000);
    const int64_t warm = warm_at() - now_us();
    if (!S.pa_on) ms = ms > PA_WARM_MS ? ms : PA_WARM_MS;
    else if (warm > ms * 1000LL) ms = (int)(warm / 1000);
    return ms;
}

esp_err_t yk_speak_chime(yk_chime_t kind) {
    if (!S.task) return ESP_ERR_INVALID_STATE;
    const size_t n = yk_chime_synth(kind, (int)S.rate, S.tone, S.rate / 5);
    esp_err_t err = yk_speak_begin();
    if (err == ESP_OK) err = yk_speak_pcm(S.tone, n);
    yk_speak_end();
    if (err == ESP_OK && !yk_speak_wait(600)) err = ESP_ERR_TIMEOUT;
    return err;
}

void yk_speak_sleep(void) {
    if (!S.task) return;  // never configured: the pin is an input, its pull-down keeps the amplifier off
    pa(false);
    gpio_hold_en(BOARD_PIN_PA_EN);
    gpio_deep_sleep_hold_en();  // digital pads keep their hold in deep sleep only with this
}

void yk_speak_volume(int level) {
    S.volume = level < 0 ? 0 : level > 10 ? 10 : level;
    if (S.open) esp_codec_dev_set_out_vol(S.spk, yk_volume_to_codec(S.volume));
}

int yk_speak_get_volume(void) { return S.volume; }

// ---------------------------------------------------------------- effects

/** The speaker sample where a sound for at_us (0: now) goes, and the next one to be mixed (yk_mix_add's start /
 *  now). Never before the amplifier is up. From a cold start the stretch begins about now, at sample 0. */
static void placement(int64_t at_us, uint64_t *start, uint64_t *now) {
    *now = S.running ? (uint64_t)S.written * S.block : 0;
    int64_t when = at_us ? at_us : now_us();
    if (when < warm_at()) when = warm_at();
    const int64_t heard = next_heard_us();
    *start = *now + (when > heard ? (uint64_t)(when - heard) * S.rate / 1000000 : 0);
}

static yk_sfx_id_t play(const yk_clip_t *clip, yk_sfx_render_fn render, void *ctx, const yk_sfx_opts_t *opts) {
    if (!S.task || S.live) {
        if (S.task) S.st.sfx_dropped++;
        return YK_SFX_BUSY;
    }
    if (!S.pa_on) pa(true);  // the 300 ms start now; the sound waits for it (yk_speak_hold_amp avoids it)
    xSemaphoreTakeRecursive(S.mix_lock, portMAX_DELAY);
    uint64_t start, now;
    placement(opts ? opts->at_us : 0, &start, &now);
    const uint32_t dropped = S.mix->dropped;
    const int id = yk_mix_add(S.mix, clip, render, ctx, opts, start, now);
    if (id > 0) S.st.sfx_started++;
    else if (S.mix->dropped != dropped) S.st.sfx_dropped++;
    xSemaphoreGiveRecursive(S.mix_lock);
    if (id > 0) xTaskNotifyGive(S.task);
    return id;
}

yk_sfx_id_t yk_sfx_play_clip(const yk_clip_t *clip, const yk_sfx_opts_t *opts) {
    if (!clip) return YK_SFX_BAD;
    return play(clip, NULL, NULL, opts);
}

yk_sfx_id_t yk_sfx_play_synth(yk_sfx_render_fn render, void *ctx, const yk_sfx_opts_t *opts) {
    if (!render) return YK_SFX_BAD;
    return play(NULL, render, ctx, opts);
}

void yk_sfx_stop(yk_sfx_id_t id, int fade_ms) {
    if (!S.task || (id <= 0 && id != 0)) return;
    xSemaphoreTakeRecursive(S.mix_lock, portMAX_DELAY);
    yk_mix_stop(S.mix, id, (uint32_t)(fade_ms > 0 ? fade_ms : 2) * S.rate / 1000);
    xSemaphoreGiveRecursive(S.mix_lock);
    // Returns once the voice is gone (its callback no longer called), unless asked from the speaker task itself.
    if (xTaskGetCurrentTaskHandle() == S.task) return;
    for (int i = 0; i < 100; i++) {
        xSemaphoreTakeRecursive(S.mix_lock, portMAX_DELAY);
        const bool on = id ? yk_mix_playing(S.mix, id) : yk_mix_active(S.mix) > 0;
        xSemaphoreGiveRecursive(S.mix_lock);
        if (!on || !S.running) return;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void yk_sfx_stop_all(int fade_ms) { yk_sfx_stop(0, fade_ms); }

void yk_sfx_set_gain(yk_sfx_id_t id, float gain, int ramp_ms) {
    if (!S.task || id <= 0) return;
    xSemaphoreTakeRecursive(S.mix_lock, portMAX_DELAY);
    yk_mix_gain(S.mix, id, gain, (uint32_t)(ramp_ms > 0 ? ramp_ms : 0) * S.rate / 1000);
    xSemaphoreGiveRecursive(S.mix_lock);
}

bool yk_sfx_playing(yk_sfx_id_t id) {
    if (!S.task || id <= 0) return false;
    xSemaphoreTakeRecursive(S.mix_lock, portMAX_DELAY);
    const bool on = yk_mix_playing(S.mix, id);
    xSemaphoreGiveRecursive(S.mix_lock);
    return on;
}

int yk_sfx_active(void) { return S.task ? sfx_active() : 0; }

void yk_speak_stats(yk_speak_stats_t *out) {
    *out = S.st;
    out->decode_us = S.st.packets ? (uint32_t)(S.decode_total / S.st.packets) : 0;
    out->mix_us = S.mix_blocks ? (uint32_t)(S.mix_total / S.mix_blocks) : 0;
    out->stack_free = S.task ? uxTaskGetStackHighWaterMark(S.task) : 0;
    if (S.mix) out->limited = S.mix->limited;
}

// ---------------------------------------------------------------- console

static void print_stats(void) {
    yk_speak_stats_t s;
    yk_speak_stats(&s);
    printf("SPEAK_STAT utterances=%u first_ms=%u warm_ms=%u open_ms=%u packets=%u errors=%u decode_us=%u "
           "decode_max_us=%u underruns=%u played_ms=%u stack_free=%u volume=%d rate=%u block_ms=%u sfx_started=%u "
           "sfx_dropped=%u sfx_max=%u mix_us=%u mix_max_us=%u late_blocks=%u limited=%u latency_ms=%d\n",
           (unsigned)s.utterances, (unsigned)s.first_ms, (unsigned)s.warm_ms, (unsigned)s.open_ms, (unsigned)s.packets,
           (unsigned)s.decode_errors, (unsigned)s.decode_us, (unsigned)s.decode_max_us, (unsigned)s.underruns,
           (unsigned)s.played_ms, (unsigned)s.stack_free, S.volume, (unsigned)s.rate, (unsigned)s.block_ms,
           (unsigned)s.sfx_started, (unsigned)s.sfx_dropped, (unsigned)s.sfx_max, (unsigned)s.mix_us,
           (unsigned)s.mix_max_us, (unsigned)s.late_blocks, (unsigned)s.limited, yk_speak_latency_ms());
}

typedef struct {
    float phase, inc, env, decay;
} test_voice_t;

/** A decaying sine for `speak sfx`: what a modal hit costs, roughly. */
static bool test_render(float *out, size_t n, const yk_sfx_block_t *blk, void *ctx) {
    (void)blk;
    test_voice_t *v = ctx;
    for (size_t i = 0; i < n; i++) {
        out[i] = 0.3f * v->env * sinf(v->phase);
        v->phase += v->inc;
        if (v->phase > 6.2831853f) v->phase -= 6.2831853f;
        v->env *= v->decay;
    }
    return v->env > 0.001f;
}

static int speak_cmd(int argc, char **argv) {
    const char *sub = argc > 1 ? argv[1] : "stat";
    if (!strcmp(sub, "tone") && argc > 3) {
        // speak tone <hz> <ms>: a sine at -12 dBFS, fed in 20 ms pieces as a stream would be
        const float hz = (float)atof(argv[2]);
        const int ms = atoi(argv[3]);
        const int piece_n = (int)S.rate / 50;
        int16_t *piece = malloc(piece_n * sizeof(int16_t));
        if (!piece) return 1;
        yk_speak_begin();
        for (int k = 0, i = 0; k < ms / 20; k++) {
            for (int j = 0; j < piece_n; j++, i++) piece[j] = (int16_t)(8192 * sinf(2 * (float)M_PI * hz * i / S.rate));
            if (yk_speak_pcm(piece, piece_n) != ESP_OK) break;
        }
        yk_speak_end();
        yk_speak_wait(ms + 1000);
        free(piece);
        print_stats();
        return 0;
    }
    if (!strcmp(sub, "sfx") && argc > 2) {
        // speak sfx <n> [ms]: n decaying sines at once (or one after another every ms), to measure the mix
        static test_voice_t v[YK_SFX_VOICES];
        const int n = atoi(argv[2]) > YK_SFX_VOICES ? YK_SFX_VOICES : atoi(argv[2]);
        const int gap = argc > 3 ? atoi(argv[3]) : 0;
        int started = 0;
        for (int i = 0; i < n; i++) {
            v[i] = (test_voice_t){.inc = 2 * (float)M_PI * (440.0f + 110.0f * i) / S.rate, .env = 1,
                                  .decay = expf(-1.0f / (0.4f * S.rate))};
            const yk_sfx_opts_t o = {.at_us = gap ? esp_timer_get_time() + (int64_t)i * gap * 1000 : 0};
            started += yk_sfx_play_synth(test_render, &v[i], &o) > 0;
        }
        const int64_t t0 = esp_timer_get_time();
        while (yk_sfx_active() && esp_timer_get_time() - t0 < 5000000) vTaskDelay(pdMS_TO_TICKS(20));
        printf("SPEAK_SFX started=%d\n", started);
        print_stats();
        return 0;
    }
    if (!strcmp(sub, "chime") && argc > 2) {
        const int64_t t0 = now_us();
        const esp_err_t err = yk_speak_chime(!strcmp(argv[2], "end") ? YK_CHIME_END : YK_CHIME_START);
        printf("SPEAK_CHIME %s ms=%lld err=%s\n", argv[2], (long long)((now_us() - t0) / 1000), esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (!strcmp(sub, "vol") && argc > 2) {
        yk_speak_volume(atoi(argv[2]));
        printf("SPEAK_VOL %d\n", S.volume);
        return 0;
    }
    if (!strcmp(sub, "hold") && argc > 2) {
        yk_speak_hold_amp(atoi(argv[2]) != 0);
        printf("SPEAK_HOLD %d\n", S.hold_amp);
        return 0;
    }
    if (!strcmp(sub, "clock")) {
        yk_speak_clock_t c;
        yk_speak_clock(&c);
        printf("SPEAK_CLOCK running=%d sample=%llu at_us=%lld latency_ms=%d\n", c.running, (unsigned long long)c.sample,
               (long long)c.at_us, yk_speak_latency_ms());
        return 0;
    }
    if (!strcmp(sub, "prepare")) {
        yk_speak_prepare();
        return 0;
    }
    if (!strcmp(sub, "stop")) {
        yk_speak_stop();
        yk_sfx_stop_all(10);
        return 0;
    }
    print_stats();
    return 0;
}

esp_err_t yk_speak_register_console(void) {
    const esp_console_cmd_t cmd = {
        .command = "speak",
        .help = "speak stat | tone <hz> <ms> | sfx <n> [gap_ms] | chime start|end | vol <0-10> | hold 0|1 | clock | "
                "prepare | stop",
        .func = speak_cmd};
    return esp_console_cmd_register(&cmd);
}

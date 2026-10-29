#include "chime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "chime";

#define RATE    AUDIO_SAMPLE_RATE
/* Above the LVGL task (6): while the ring animates, the renderer must not be able to starve the
 * audio writes, or the I2S buffer runs dry and the sound stutters. */
#define CHIME_TASK_PRIORITY 10
#define VOLUME_MAX 92 /* codec output volume, 0..100 (70 was about -15 dB: too quiet for a desk speaker) */
static volatile int s_volume = VOLUME_MAX; /* codec volume for the user's 0..100 (chime_set_volume) */

typedef struct {
    float freq_hz;
    float start_s;
    float length_s; /* ring-out time; the note decays exponentially inside it */
    float gain;
} note_t;

/* C6 E6 G6 C7 arpeggio */
static const note_t FOCUS_DONE[] = {
    { 1046.5f, 0.00f, 1.20f, 0.36f },
    { 1318.5f, 0.16f, 1.20f, 0.34f },
    { 1568.0f, 0.32f, 1.30f, 0.32f },
    { 2093.0f, 0.48f, 1.60f, 0.26f },
};
/* G5 -> C5, gentler */
static const note_t BREAK_DONE[] = {
    { 784.0f, 0.00f, 1.10f, 0.42f },
    { 523.3f, 0.22f, 1.50f, 0.42f },
};
static const note_t PREVIEW[] = {
    { 1174.7f, 0.00f, 0.70f, 0.50f },
};

static QueueHandle_t     s_queue;
static TaskHandle_t      s_task;
static SemaphoreHandle_t s_play_lock;

/* ------------------------------------------------------------- synthesis */

/* Mono float mix -> stereo int16 buffer (PSRAM). */
static int16_t *to_stereo(const float *mix, size_t frames)
{
    int16_t *pcm = heap_caps_malloc(frames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm) {
        return NULL;
    }
    for (size_t i = 0; i < frames; i++) {
        float v = mix[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        const int16_t s16 = (int16_t)(v * 30000.0f);
        pcm[2 * i]     = s16;
        pcm[2 * i + 1] = s16;
    }
    return pcm;
}

static int16_t *synth_notes(const note_t *notes, size_t n_notes, size_t *out_frames)
{
    float total_s = 0;
    for (size_t i = 0; i < n_notes; i++) {
        const float end = notes[i].start_s + notes[i].length_s;
        if (end > total_s) total_s = end;
    }
    const size_t frames = (size_t)(total_s * RATE) + 1;
    float *mix = heap_caps_calloc(frames, sizeof(float), MALLOC_CAP_SPIRAM);
    if (!mix) {
        return NULL;
    }

    /* Each note = 3 partials (fundamental, 2x, 2.76x) with exponential decay.
     * Rotating phasors + a per-sample decay factor need a few multiplies per
     * sample instead of sinf()/expf(), which starved the watchdog. */
    static const float partial_ratio[3] = { 1.0f, 2.0f, 2.76f };
    static const float partial_gain[3]  = { 1.0f, 0.30f, 0.10f };
    static const float partial_decay[3] = { 1.0f, 3.0f, 5.0f }; /* x the base decay rate */
    const float two_pi = 6.2831853f;

    for (size_t k = 0; k < n_notes; k++) {
        const note_t *nt       = &notes[k];
        const size_t  first    = (size_t)(nt->start_s * RATE);
        const size_t  count    = (size_t)(nt->length_s * RATE);
        const float   tau      = nt->length_s / 4.5f;
        const float   base     = expf(-1.0f / (RATE * tau));
        const float   attack_n = 0.006f * RATE;

        float c[3], s[3], dc[3], ds[3], env[3], dec[3];
        for (int p = 0; p < 3; p++) {
            const float w = two_pi * nt->freq_hz * partial_ratio[p] / RATE;
            c[p] = 1.0f; s[p] = 0.0f; dc[p] = cosf(w); ds[p] = sinf(w);
            env[p] = 1.0f; dec[p] = powf(base, partial_decay[p]);
        }
        for (size_t i = 0; i < count && first + i < frames; i++) {
            float v = 0;
            for (int p = 0; p < 3; p++) {
                v += partial_gain[p] * s[p] * env[p];
                const float nc = c[p] * dc[p] - s[p] * ds[p];
                s[p] = s[p] * dc[p] + c[p] * ds[p];
                c[p] = nc;
                env[p] *= dec[p];
            }
            const float attack = (float)i < attack_n ? (float)i / attack_n : 1.0f;
            mix[first + i] += nt->gain * attack * v;
            if ((i & 0x3FF) == 0) {
                vTaskDelay(1); /* let the idle task (watchdog) run */
            }
        }
    }

    int16_t *pcm = to_stereo(mix, frames);
    free(mix);
    *out_frames = frames;
    return pcm;
}

/* Steady sine with short fades: for measuring, not for listening. */
static int16_t *synth_tone(int freq_hz, int ms, size_t *out_frames)
{
    const size_t frames = (size_t)ms * RATE / 1000;
    float *mix = heap_caps_malloc(frames * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!mix) {
        return NULL;
    }
    const float w  = 6.2831853f * freq_hz / RATE;
    const float dc = cosf(w), ds = sinf(w);
    float c = 1.0f, s = 0.0f;
    const size_t fade = RATE / 100; /* 10 ms */
    for (size_t i = 0; i < frames; i++) {
        float g = 0.6f;
        if (i < fade) g *= (float)i / fade;
        if (frames - i < fade) g *= (float)(frames - i) / fade;
        mix[i] = g * s;
        const float nc = c * dc - s * ds;
        s = s * dc + c * ds;
        c = nc;
        if ((i & 0x3FF) == 0) vTaskDelay(1);
    }
    int16_t *pcm = to_stereo(mix, frames);
    free(mix);
    *out_frames = frames;
    return pcm;
}

/* ------------------------------------------------------ amplifier + play */

/* The speaker amplifier is enabled by a GPIO on this board. Which one is
 * established by `hw loopback` / `soundtest`; GPIO43 is the default because the
 * factory firmware drives it as an output (low when idle). */
#define PA_MAX_PINS 2
static int s_pa_pins[PA_MAX_PINS] = { 43, -1 };

static void pa_set(bool on)
{
    for (int i = 0; i < PA_MAX_PINS; i++) {
        if (s_pa_pins[i] >= 0) {
            gpio_set_level(s_pa_pins[i], on ? 1 : 0);
        }
    }
}

/* Only the pins in use are driven. Tikpal configured both candidates (43 and 46) on every init, which drove
 * GPIO46 (a strapping pin whose net on this board is not documented) low for no reason; it is now touched only
 * while `soundtest` tries it. A pin given up goes back to its reset state. */
static void pa_pin_setup(int pin)
{
    if (pin < 0) return;
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT); /* input too, so it can be read back */
    gpio_set_level(pin, 0);
}

void chime_set_pa_pins(int a, int b)
{
    for (int i = 0; i < PA_MAX_PINS; i++) {
        const int old = s_pa_pins[i];
        if (old >= 0 && old != a && old != b) gpio_reset_pin(old);
    }
    pa_pin_setup(a);
    if (b != a) pa_pin_setup(b);
    s_pa_pins[0] = a;
    s_pa_pins[1] = b;
}

void chime_get_pa_pins(int *a, int *b)
{
    *a = s_pa_pins[0];
    *b = s_pa_pins[1];
}

static void play_pcm(int16_t *pcm, size_t frames, int volume)
{
    esp_codec_dev_handle_t spk = audio_speaker();
    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = RATE,
        .channel         = 2,
        .bits_per_sample = 16,
    };
    /* esp_codec_dev's set_fmt() disables the I2S channel before reconfiguring it;
     * harmless, but IDF logs an error for it. Keep that out of the log. */
    esp_log_level_set("i2s_common", ESP_LOG_NONE);
    const int opened = esp_codec_dev_open(spk, &fs);
    esp_log_level_set("i2s_common", ESP_LOG_ERROR);
    if (opened != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "codec open failed: %d", opened);
        return;
    }
    esp_codec_dev_set_out_vol(spk, volume);
    pa_set(true);
    vTaskDelay(pdMS_TO_TICKS(20)); /* let the amplifier settle before the first sample */
    const size_t total = frames * 2 * sizeof(int16_t);
    const size_t chunk = 4096;
    for (size_t off = 0; off < total; off += chunk) {
        const size_t len = total - off < chunk ? total - off : chunk;
        if (esp_codec_dev_write(spk, (uint8_t *)pcm + off, (int)len) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "codec write failed");
            break;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(60)); /* drain the DMA buffers before muting */
    pa_set(false);
    esp_codec_dev_close(spk);
}

static void play_notes(const note_t *notes, size_t n, int volume)
{
    xSemaphoreTake(s_play_lock, portMAX_DELAY);
    size_t   frames = 0;
    int16_t *pcm    = synth_notes(notes, n, &frames);
    if (pcm) {
        play_pcm(pcm, frames, volume);
        free(pcm);
    } else {
        ESP_LOGE(TAG, "out of memory for chime buffer");
    }
    xSemaphoreGive(s_play_lock);
}

void chime_play_tone(int freq_hz, int ms, int volume)
{
    if (!s_play_lock) {
        return;
    }
    xSemaphoreTake(s_play_lock, portMAX_DELAY);
    size_t   frames = 0;
    int16_t *pcm    = synth_tone(freq_hz, ms, &frames);
    if (pcm) {
        play_pcm(pcm, frames, volume);
        free(pcm);
    }
    xSemaphoreGive(s_play_lock);
}

/* Key ES8311 registers, to compare a playback that is audible with one that is not. */
void chime_dump_regs(const char *when)
{
    const audio_codec_ctrl_if_t *ctrl = audio_speaker_ctrl();
    if (!ctrl) {
        return;
    }
    static const uint8_t regs[] = { 0x00, 0x01, 0x02, 0x06, 0x09, 0x0A, 0x0D, 0x0E, 0x12, 0x13, 0x31, 0x32, 0x37, 0x44, 0xFD, 0xFE, 0xFF };
    char line[200];
    int  len = snprintf(line, sizeof(line), "ES8311 %-14s", when);
    for (size_t i = 0; i < sizeof(regs) && len < (int)sizeof(line) - 10; i++) {
        uint8_t v = 0;
        if (ctrl->read_reg(ctrl, regs[i], 1, &v, 1) == 0) {
            len += snprintf(line + len, sizeof(line) - len, " %02X=%02X", regs[i], v);
        } else {
            len += snprintf(line + len, sizeof(line) - len, " %02X=??", regs[i]);
        }
    }
    ESP_LOGI(TAG, "%s", line);
}

/* ------------------------------------------------------------------ task */

/* The three chimes are synthesised once, at boot, so that when a phase ends the
 * only work left is copying samples to the codec: nothing that competes with the
 * UI at the busiest moment, and no synthesis delay before the sound starts. */
typedef struct {
    const note_t *notes;
    size_t        n;
    int16_t      *pcm;
    size_t        frames;
} cached_chime_t;

static cached_chime_t s_chimes[] = {
    [CHIME_FOCUS_DONE] = { FOCUS_DONE, sizeof(FOCUS_DONE) / sizeof(FOCUS_DONE[0]) },
    [CHIME_BREAK_DONE] = { BREAK_DONE, sizeof(BREAK_DONE) / sizeof(BREAK_DONE[0]) },
    [CHIME_PREVIEW]    = { PREVIEW, sizeof(PREVIEW) / sizeof(PREVIEW[0]) },
};

static void chime_task(void *arg)
{
    (void)arg;
    for (size_t i = 0; i < sizeof(s_chimes) / sizeof(s_chimes[0]); i++) {
        s_chimes[i].pcm = synth_notes(s_chimes[i].notes, s_chimes[i].n, &s_chimes[i].frames);
        if (!s_chimes[i].pcm) {
            ESP_LOGW(TAG, "chime %d not cached (out of memory), will be synthesised on demand", (int)i);
        }
    }

    chime_kind_t kind;
    for (;;) {
        if (xQueueReceive(s_queue, &kind, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if ((size_t)kind >= sizeof(s_chimes) / sizeof(s_chimes[0])) {
            kind = CHIME_PREVIEW;
        }
        const cached_chime_t *c = &s_chimes[kind];
        if (c->pcm) {
            xSemaphoreTake(s_play_lock, portMAX_DELAY);
            if (s_volume > 0) play_pcm(c->pcm, c->frames, s_volume);
            xSemaphoreGive(s_play_lock);
        } else {
            if (s_volume > 0) play_notes(c->notes, c->n, s_volume);
        }
    }
}

esp_err_t chime_init(void)
{
    ESP_RETURN_ON_ERROR(audio_init(), TAG, "audio");
    /* The chimes are made and played at RATE; a port already running at another rate would play them wrong. */
    ESP_RETURN_ON_FALSE(audio_sample_rate() == RATE, ESP_ERR_INVALID_STATE, TAG, "audio runs at %u Hz, not %d",
                        (unsigned)audio_sample_rate(), RATE);
    ESP_RETURN_ON_FALSE(audio_speaker(), ESP_FAIL, TAG, "no speaker codec");

    s_play_lock = xSemaphoreCreateMutex();
    chime_set_pa_pins(s_pa_pins[0], s_pa_pins[1]);

    s_queue = xQueueCreate(2, sizeof(chime_kind_t));
    if (!s_queue || xTaskCreate(chime_task, "chime", 6144, NULL, CHIME_TASK_PRIORITY, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready (%d Hz stereo, amplifier pin GPIO%d)", RATE, s_pa_pins[0]);
    return ESP_OK;
}

void chime_set_priority(int priority)
{
    if (s_task) {
        vTaskPrioritySet(s_task, priority);
    }
}

bool chime_available(void)
{
    return s_queue != NULL;
}

static volatile bool s_muted;

void chime_set_volume(int percent)
{
    /* the codec's scale is roughly in dB: map 1..100 onto its audible top part, 0 = silent */
    if (percent <= 0) {
        s_volume = 0;
    } else {
        if (percent > 100) percent = 100;
        s_volume = 60 + percent * (VOLUME_MAX - 60) / 100;
    }
}

void chime_set_muted(bool muted)
{
    s_muted = muted;
}

void chime_play(chime_kind_t kind)
{
    if (s_queue && !s_muted) {
        xQueueSend(s_queue, &kind, 0);
    }
}

/* Plays the same tone four times with a different amplifier-enable pin each
 * time, so a listener can tell which pin actually opens the speaker. */
void chime_soundtest(void)
{
    static const struct { int a, b; const char *label; } steps[] = {
        { 43, -1, "GPIO43 high" },
        { 46, -1, "GPIO46 high" },
        { 43, 46, "GPIO43 + GPIO46 high" },
        { -1, -1, "no amplifier pin (codec only)" },
    };
    int saved_a, saved_b;
    chime_get_pa_pins(&saved_a, &saved_b);
    for (int i = 0; i < 4; i++) {
        chime_set_pa_pins(steps[i].a, steps[i].b);
        printf("SOUNDTEST %d/4: %s\n", i + 1, steps[i].label);
        chime_play_tone(1000, 1000, 80);
        vTaskDelay(pdMS_TO_TICKS(900));
    }
    chime_set_pa_pins(saved_a, saved_b);
}

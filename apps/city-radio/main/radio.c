#include "radio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decoder/esp_audio_dec_default.h"
#include "esp_audio_types.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "pcm_conv.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"
#include "stations.h"
#include "yaykit_speak.h"
#include "yaykit_wifi.h"

static const char *TAG = "radio";

#define READ_BYTES 4096
#define HTTP_BUFFER 8192           // over CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4 KB), so it lands in PSRAM
// Sized for the 32 kbps streams (4 KB a second).
#define NET_BUFFER (96 * 1024)     // about 24 s, in PSRAM
#define START_BYTES (24 * 1024)    // about 6 s held before the first sound (2 s of it go straight to the speaker)
#define REFILL_BYTES (16 * 1024)   // about 4 s held after the buffer ran dry
#define RETRY_MS 3000
#define RECONNECT_MAX_MS 5000      // the reader's back-off between failed connections
#define TASK_STACK 8192

static struct {
    volatile int station;
    volatile bool want;
    volatile uint32_t gen;  // bumped on every request: the tasks drop what they do when it changes
    volatile radio_state_t state;
    radio_stats_t stats;
    TaskHandle_t task;
    // The reader: keeps `net_url` flowing into `net` (connecting again whenever the stream drops) while `net_id`
    // stays what it was given; gives `net_done` once it has let go of the stream.
    TaskHandle_t net_task;
    StreamBufferHandle_t net;
    SemaphoreHandle_t net_done;
    const char *volatile net_url;
    volatile uint32_t net_id, net_seq;
    volatile bool net_failing;  // the last connection attempt failed
} S;

static const char *const NAMES[] = {"idle", "wait_net", "connecting", "playing", "buffering", "retrying"};

const char *radio_state_name(radio_state_t s) { return s <= RADIO_RETRYING ? NAMES[s] : "?"; }
radio_state_t radio_state(void) { return S.state; }
int radio_station(void) { return S.station; }

void radio_stats(radio_stats_t *out) {
    *out = S.stats;
    out->buffered = S.net ? (uint32_t)xStreamBufferBytesAvailable(S.net) : 0;
}

void radio_play(int station) {
    S.station = station_step(station, 0);
    S.want = true;
    S.gen++;
    if (S.task) xTaskNotifyGive(S.task);
}

void radio_stop(void) {
    S.want = false;
    S.gen++;
    if (S.task) xTaskNotifyGive(S.task);
}

/** Sleeps up to ms, or less when a new request comes in. False when the request changed. */
static bool wait_same(uint32_t gen, int ms) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
    return gen == S.gen;
}

/** Opens the stream, following up to 3 redirects. NULL when it failed or the reader was told to stop. */
static esp_http_client_handle_t open_stream(const char *url, uint32_t id) {
    const esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 3000,  // a stream that stalls this long is opened again
        .buffer_size = HTTP_BUFFER,
        .user_agent = "yaykit-city-radio/" APP_VERSION,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    for (int hop = 0; hop < 4 && id == S.net_id; hop++) {
        if (esp_http_client_open(c, 0) != ESP_OK) break;
        esp_http_client_fetch_headers(c);
        const int status = esp_http_client_get_status_code(c);
        if (status == 200) return c;
        if (status >= 300 && status < 400 && esp_http_client_set_redirection(c) == ESP_OK) {
            S.stats.redirects++;
            esp_http_client_close(c);
            continue;
        }
        ESP_LOGW(TAG, "HTTP %d", status);
        break;
    }
    esp_http_client_cleanup(c);
    return NULL;
}

/** Sleeps up to ms in short steps; false once the reader was told to stop. */
static bool net_wait(uint32_t id, int ms) {
    for (; ms > 0 && id == S.net_id; ms -= 100) vTaskDelay(pdMS_TO_TICKS(100));
    return id == S.net_id;
}

/** The reader. A dropped stream is opened again at once and appended to the buffer, so the decoder keeps playing
 *  what is buffered meanwhile (the MP3 parser finds the next frame across the seam). */
static void net_task(void *arg) {
    (void)arg;
    uint8_t *buf = heap_caps_malloc(READ_BYTES, MALLOC_CAP_SPIRAM);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint32_t id = S.net_id;
        const char *url = S.net_url;
        int backoff = 1000;
        while (buf && id == S.net_id) {
            esp_http_client_handle_t http = open_stream(url, id);
            if (!http) {
                S.net_failing = true;
                S.stats.failures++;
                printf("CITYRADIO_RETRY failures=%u wait_ms=%d\n", (unsigned)S.stats.failures, backoff);
                net_wait(id, backoff);
                backoff = backoff * 2 > RECONNECT_MAX_MS ? RECONNECT_MAX_MS : backoff * 2;
                continue;
            }
            S.net_failing = false;
            S.stats.connects++;
            backoff = 1000;
            while (id == S.net_id) {
                const int n = esp_http_client_read(http, (char *)buf, READ_BYTES);
                if (n <= 0) {
                    printf("CITYRADIO_RECONNECT read=%d buffered=%u\n", n, (unsigned)xStreamBufferBytesAvailable(S.net));
                    break;
                }
                S.stats.bytes += n;
                size_t sent = 0;
                while (sent < (size_t)n && id == S.net_id)
                    sent += xStreamBufferSend(S.net, buf + sent, n - sent, pdMS_TO_TICKS(200));
            }
            esp_http_client_close(http);
            esp_http_client_cleanup(http);
        }
        S.net_failing = false;
        xSemaphoreGive(S.net_done);
    }
}

/** How much the speaker still has to play, in samples. `pushed` counts what was handed to it; whenever the speaker
 *  ran dry (its clock counts the silence too) or reopened, the count starts again from what is heard now. */
typedef struct {
    uint64_t pushed, heard;
} speaker_track_t;

static int64_t speaker_ahead(speaker_track_t *t) {
    yk_speak_clock_t c;
    yk_speak_clock(&c);
    if (!c.running) {
        t->pushed = t->heard = 0;
        return 0;
    }
    if (c.sample < t->heard || t->pushed < c.sample) t->pushed = c.sample;
    t->heard = c.sample;
    return (int64_t)(t->pushed - c.sample);
}

/** Plays one station until the request changes (returns true) or the stream fails (false). */
static bool play_stream(const station_t *st, uint32_t gen) {
    S.state = RADIO_CONNECTING;
    S.stats.bytes = 0;
    S.stats.rebuffers = 0;
    printf("CITYRADIO_CONNECT station=\"%s\"\n", st->name);
    // Modem sleep (yaykit_wifi's default) slows a steady stream down below real time: off while playing.
    esp_wifi_set_ps(WIFI_PS_NONE);
    // Hand the station to the reader (idle now: the previous station's reader gave net_done).
    xStreamBufferReset(S.net);
    S.net_url = st->url;
    S.net_id = ++S.net_seq;
    xTaskNotifyGive(S.net_task);

    esp_audio_simple_dec_cfg_t dcfg = {.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3};
    esp_audio_simple_dec_handle_t dec = NULL;
    uint8_t *in = heap_caps_malloc(READ_BYTES, MALLOC_CAP_SPIRAM);
    uint32_t out_cap = 4608;  // one MP3 frame: 1152 stereo 16-bit samples
    uint8_t *out = heap_caps_malloc(out_cap, MALLOC_CAP_SPIRAM);
    size_t mono_cap = 4096;
    int16_t *mono = heap_caps_malloc(mono_cap * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    bool ok = in && out && mono && esp_audio_simple_dec_open(&dcfg, &dec) == ESP_AUDIO_ERR_OK;

    pcm_conv_t conv = {0};
    const uint32_t rate = yk_speak_rate();
    bool begun = false, filling = true;
    speaker_track_t track = {0};
    size_t hold = START_BYTES;
    while (ok && gen == S.gen) {
        if (filling) {
            if (xStreamBufferBytesAvailable(S.net) < hold) {
                S.state = S.net_failing ? RADIO_RETRYING : begun ? RADIO_BUFFERING : RADIO_CONNECTING;
                wait_same(gen, 50);
                continue;
            }
            filling = false;
        }
        const size_t n = xStreamBufferReceive(S.net, in, READ_BYTES, pdMS_TO_TICKS(200));
        if (!n) {
            // Nothing new yet, but the speaker still holds up to 2 s: keep waiting. Only when it has played out
            // too did the stream really run dry: then wait for a couple of seconds of stream before going on.
            if (!begun) continue;
            if (speaker_ahead(&track) > (int64_t)rate / 5) continue;  // over 0.2 s still to play
            filling = true;
            hold = REFILL_BYTES;
            S.stats.rebuffers++;
            S.state = RADIO_BUFFERING;
            printf("CITYRADIO_BUFFERING rebuffers=%u\n", (unsigned)S.stats.rebuffers);
            continue;
        }
        esp_audio_simple_dec_raw_t raw = {.buffer = in, .len = (uint32_t)n};
        while (raw.len > 0 && gen == S.gen) {
            esp_audio_simple_dec_out_t frame = {.buffer = out, .len = out_cap};
            const esp_audio_err_t err = esp_audio_simple_dec_process(dec, &raw, &frame);
            if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                uint8_t *bigger = heap_caps_realloc(out, frame.needed_size, MALLOC_CAP_SPIRAM);
                if (!bigger) {
                    ok = false;
                    break;
                }
                out = bigger, out_cap = frame.needed_size;
                continue;
            }
            if (err != ESP_AUDIO_ERR_OK) {
                S.stats.decode_errors++;  // a damaged frame: skip this read, the parser finds the next frame
                break;
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            if (!frame.decoded_size) continue;
            esp_audio_simple_dec_info_t info;
            esp_audio_simple_dec_get_info(dec, &info);
            if (info.sample_rate != conv.in_rate || info.channel != S.stats.in_channels) {
                pcm_conv_init(&conv, info.sample_rate, rate);
                S.stats.in_rate = info.sample_rate, S.stats.in_channels = info.channel;
                printf("CITYRADIO_FORMAT rate=%u channels=%u bitrate=%u\n", (unsigned)info.sample_rate,
                       (unsigned)info.channel, (unsigned)info.bitrate);
            }
            const int channels = info.channel == 2 ? 2 : 1;
            const size_t frames = frame.decoded_size / (sizeof(int16_t) * channels);
            const size_t need = pcm_conv_max_out(&conv, frames);
            if (need > mono_cap) {
                int16_t *bigger = heap_caps_realloc(mono, need * sizeof(int16_t), MALLOC_CAP_SPIRAM);
                if (!bigger) {
                    ok = false;
                    break;
                }
                mono = bigger, mono_cap = need;
            }
            const size_t m = pcm_conv_run(&conv, (const int16_t *)out, frames, channels, mono, mono_cap);
            if (!begun) {
                yk_speak_begin();
                begun = true;
            }
            // Blocks while 2 s are waiting in the speaker's ring: that paces decoding to the speaker.
            while (gen == S.gen && yk_speak_pcm(mono, m) == ESP_ERR_TIMEOUT) {
            }
            speaker_ahead(&track);
            track.pushed += m;
            if (S.state != RADIO_PLAYING && gen == S.gen) {
                S.state = RADIO_PLAYING;
                printf("CITYRADIO_PLAYING station=\"%s\" buffered=%u\n", st->name,
                       (unsigned)xStreamBufferBytesAvailable(S.net));
            }
        }
    }
    if (begun) yk_speak_stop();
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    // Stop the reader (it is between reads within 200 ms, or blocked in a read for at most its 3 s timeout).
    S.net_id = 0;
    xSemaphoreTake(S.net_done, portMAX_DELAY);
    if (dec) esp_audio_simple_dec_close(dec);
    free(in);
    free(out);
    free(mono);
    return gen != S.gen;
}

static void radio_task(void *arg) {
    (void)arg;
    for (;;) {
        const uint32_t gen = S.gen;
        if (!S.want) {
            S.state = RADIO_IDLE;
            wait_same(gen, portMAX_DELAY);
            continue;
        }
        if (yk_wifi_state() != YK_WIFI_CONNECTED) {
            S.state = RADIO_WAIT_NET;
            if (!yk_wifi_wait_connected(1000)) continue;
        }
        if (play_stream(station_at(S.station), gen)) continue;  // the request changed
        ESP_LOGE(TAG, "the decoder did not start");  // out of memory: the reader reconnects by itself otherwise
        S.state = RADIO_RETRYING;
        wait_same(gen, RETRY_MS);
    }
}

esp_err_t radio_init(void) {
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    S.net = xStreamBufferCreateWithCaps(NET_BUFFER, 1, MALLOC_CAP_SPIRAM);
    S.net_done = xSemaphoreCreateBinary();
    if (!S.net || !S.net_done) return ESP_ERR_NO_MEM;
    // PSRAM stacks: these tasks never write flash (NVS is written from the LVGL task).
    if (xTaskCreatePinnedToCoreWithCaps(net_task, "radio_net", TASK_STACK, NULL, 6, &S.net_task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCoreWithCaps(radio_task, "radio", TASK_STACK, NULL, 5, &S.task, 1, MALLOC_CAP_SPIRAM) !=
        pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

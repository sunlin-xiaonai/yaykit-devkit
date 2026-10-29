// Opus for the microphone: 16 kHz mono, 60 ms packets (docs/design/yay-device.md 4.2, 6.6), through
// esp_audio_codec's encoder (libopus).
#include <stdlib.h>
#include <string.h>

#include "encoder/impl/esp_opus_enc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "yaykit_audio.h"

#define SAMPLES 960  // 60 ms
#define MAX_PACKET 512

struct yk_opus_enc {
    void *h;
    int16_t pcm[SAMPLES];
    size_t have;
    uint8_t packet[MAX_PACKET];
    uint32_t last_us;
    size_t bytes;
};

yk_opus_enc_t *yk_opus_enc_create(int complexity, int bitrate) {
    yk_opus_enc_t *e = heap_caps_calloc(1, sizeof(*e), MALLOC_CAP_SPIRAM);
    if (!e) return NULL;
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = 16000;
    cfg.channel = 1;
    cfg.bits_per_sample = 16;
    cfg.bitrate = bitrate > 0 ? bitrate : 16000;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity = complexity < 0 ? 0 : complexity > 10 ? 10 : complexity;
    cfg.enable_vbr = true;
    if (esp_opus_enc_open(&cfg, sizeof(cfg), &e->h) != ESP_AUDIO_ERR_OK) {
        heap_caps_free(e);
        return NULL;
    }
    return e;
}

int yk_opus_enc_feed(yk_opus_enc_t *e, const int16_t *pcm, size_t samples, yk_opus_packet_fn on_packet, void *ctx) {
    int packets = 0;
    while (samples) {
        size_t n = SAMPLES - e->have < samples ? SAMPLES - e->have : samples;
        memcpy(e->pcm + e->have, pcm, n * sizeof(int16_t));
        e->have += n, pcm += n, samples -= n;
        if (e->have < SAMPLES) break;
        esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)e->pcm, .len = sizeof(e->pcm)};
        esp_audio_enc_out_frame_t out = {.buffer = e->packet, .len = sizeof(e->packet)};
        const int64_t t0 = esp_timer_get_time();
        if (esp_opus_enc_process(e->h, &in, &out) != ESP_AUDIO_ERR_OK) return -1;
        e->last_us = (uint32_t)(esp_timer_get_time() - t0);
        e->have = 0;
        e->bytes += out.encoded_bytes;
        packets++;
        if (on_packet) on_packet(e->packet, out.encoded_bytes, ctx);
    }
    return packets;
}

void yk_opus_enc_reset(yk_opus_enc_t *e) {
    e->have = 0;
    esp_opus_enc_reset(e->h);
}

uint32_t yk_opus_enc_last_us(const yk_opus_enc_t *e) { return e->last_us; }
size_t yk_opus_enc_bytes(const yk_opus_enc_t *e) { return e->bytes; }

void yk_opus_enc_destroy(yk_opus_enc_t *e) {
    if (!e) return;
    esp_opus_enc_close(e->h);
    heap_caps_free(e);
}

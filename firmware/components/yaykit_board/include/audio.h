/*
 * Audio hardware: one I2S port (I2S0) shared by
 *   - ES8311 DAC -> amplifier -> speaker      (transmit, standard I2S, stereo)
 *   - ES7210 4-channel ADC <- 4 microphones   (receive, 4-slot TDM)
 * Both directions use the same MCLK/BCLK/WS pins and the same sample rate, and
 * both I2S channels stay enabled; the codec devices open/close on top of that.
 * The rate is chosen once, at start: 24 kHz (the default) or 16 kHz (speech).
 * Open the codec devices at audio_sample_rate().
 */
#pragma once

#include <stdint.h>

#include "audio_codec_ctrl_if.h"
#include "esp_codec_dev.h"
#include "esp_err.h"

#define AUDIO_SAMPLE_RATE 24000 /* the default rate; audio_init_rate() picks another */
#define AUDIO_MIC_SLOTS   4

/* Same as audio_init_rate(AUDIO_SAMPLE_RATE): ESP_OK if audio runs at 24 kHz, ESP_ERR_INVALID_STATE if it
 * already runs at another rate. */
esp_err_t audio_init(void);

/* Starts audio at 16000 or 24000 Hz. ESP_ERR_INVALID_STATE if it already runs at another rate. */
esp_err_t audio_init_rate(uint32_t sample_rate);

/* The I2S DMA buffer (P13): how far the speaker runs behind what is written to it. One buffer serves both
 * directions (they share a port and must be created together), so a shallow one also leaves the microphones less
 * slack against a busy CPU. Default 8 frames of 20 ms (160 ms): what yaykit_audio's capture was measured with.
 * Party Play's low-latency sound uses 4 x 5 ms, Breathe 12 x 5 ms (60 ms). */
typedef struct {
    uint32_t sample_rate;  /* 16000 or 24000 */
    uint8_t  frame_ms;     /* one DMA frame: 5, 10 or 20 (0 = 20) */
    uint8_t  frames;       /* DMA frames: 2..16 (0 = 8) */
} audio_config_t;
/* Starts audio with these settings. When audio already runs: ESP_ERR_INVALID_STATE for another rate; another DMA
 * size is kept as it is (logged), so read audio_dma_frame_samples() / audio_dma_frames() rather than assume. */
esp_err_t audio_init_cfg(const audio_config_t *cfg);
/* Samples (per channel) in one DMA frame, and the number of frames; 0 until audio started. */
uint32_t audio_dma_frame_samples(void);
uint32_t audio_dma_frames(void);

/* The running rate, 0 until audio started. */
uint32_t audio_sample_rate(void);

/* NULL until audio_init() succeeded. */
esp_codec_dev_handle_t audio_speaker(void);
esp_codec_dev_handle_t audio_mic(void);

/* Control interface of the ES8311 (for register-level diagnostics). */
const audio_codec_ctrl_if_t *audio_speaker_ctrl(void);

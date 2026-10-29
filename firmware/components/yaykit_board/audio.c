#include "audio.h"

#include "board.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "yk_board_logic.h"

static const char *TAG = "audio";

static esp_codec_dev_handle_t        s_spk;
static esp_codec_dev_handle_t        s_mic;
static const audio_codec_ctrl_if_t *s_spk_ctrl;
static uint32_t                      s_rate; /* 0 until started */
static uint32_t                      s_frame_samples, s_frames;

esp_err_t audio_init(void)
{
    return audio_init_rate(AUDIO_SAMPLE_RATE);
}

uint32_t audio_sample_rate(void)
{
    return s_rate;
}

uint32_t audio_dma_frame_samples(void)
{
    return s_frame_samples;
}

uint32_t audio_dma_frames(void)
{
    return s_frames;
}

esp_err_t audio_init_rate(uint32_t sample_rate)
{
    return audio_init_cfg(&(audio_config_t){ .sample_rate = sample_rate });
}

esp_err_t audio_init_cfg(const audio_config_t *cfg)
{
    const uint32_t sample_rate = cfg->sample_rate;
    const uint32_t frame_ms = cfg->frame_ms == 5 || cfg->frame_ms == 10 ? cfg->frame_ms : 20;
    const uint32_t frames = cfg->frames ? (cfg->frames < 2 ? 2 : cfg->frames > 16 ? 16 : cfg->frames) : 8;
    if (s_rate) {
        /* TX and RX share BCLK/WS: the port cannot run at two rates. */
        if (s_rate != sample_rate) return ESP_ERR_INVALID_STATE;
        if (s_frame_samples != sample_rate * frame_ms / 1000 || s_frames != frames) {
            ESP_LOGW(TAG, "already running with %u x %u samples of DMA; keeping them", (unsigned)s_frames,
                     (unsigned)s_frame_samples);
        }
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(yk_board_audio_rate_ok(sample_rate), ESP_ERR_INVALID_ARG, TAG, "%u Hz is not supported",
                        (unsigned)sample_rate);
    ESP_RETURN_ON_ERROR(board_init(), TAG, "board");

    /* esp_codec_dev logs every open/close at INFO; keep the console readable */
    esp_log_level_set("I2S_IF", ESP_LOG_WARN);
    esp_log_level_set("Adev_Codec", ESP_LOG_WARN);

    /* One port, both directions, same clocks. This mirrors the reference
     * board's working configuration (TX standard stereo + RX 4-slot TDM,
     * MCLK = 256 x fs, RX BCLK divider 8). */
    i2s_chan_handle_t tx = NULL, rx = NULL;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.auto_clear    = true;
    chan.dma_desc_num  = frames;                          /* default 8 x 20 ms = 160 ms of headroom against UI load */
    chan.dma_frame_num = sample_rate * frame_ms / 1000;   /* 20 ms: 480 frames at 24 kHz, 320 at 16 kHz */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &tx, &rx), TAG, "i2s channels");

    const i2s_std_gpio_config_t std_gpio = {
        .mclk = BOARD_PIN_I2S_MCLK, .bclk = BOARD_PIN_I2S_BCLK, .ws = BOARD_PIN_I2S_WS,
        .dout = BOARD_PIN_I2S_DOUT, .din = I2S_GPIO_UNUSED,
    };
    i2s_std_config_t tx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = std_gpio,
    };
    tx_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    i2s_tdm_config_t rx_cfg = {
        .clk_cfg  = I2S_TDM_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                                                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
        .gpio_cfg = {
            .mclk = BOARD_PIN_I2S_MCLK, .bclk = BOARD_PIN_I2S_BCLK, .ws = BOARD_PIN_I2S_WS,
            .dout = I2S_GPIO_UNUSED, .din = BOARD_PIN_I2S_DIN,
        },
    };
    rx_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    rx_cfg.clk_cfg.bclk_div      = 8;
    rx_cfg.slot_cfg.total_slot   = AUDIO_MIC_SLOTS;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &tx_cfg), TAG, "tx init");
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(rx, &rx_cfg), TAG, "rx init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG, "tx enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "rx enable");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && gpio_if, ESP_FAIL, TAG, "data interface");

    /* ---- ES8311 speaker codec (I2C 0x18) ---- */
    audio_codec_i2c_cfg_t spk_i2c = {
        .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = board_i2c(),
    };
    s_spk_ctrl = audio_codec_new_i2c_ctrl(&spk_i2c);
    ESP_RETURN_ON_FALSE(s_spk_ctrl, ESP_FAIL, TAG, "ES8311 control");
    es8311_codec_cfg_t es8311 = {
        .ctrl_if    = s_spk_ctrl,
        .gpio_if    = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin     = -1, /* not the library: yaykit_speak switches the amplifier (BOARD_PIN_PA_EN); at 24 kHz
                           * yaykit_chime and Breathe still do it themselves (a stopgap, not for new code) */
        .use_mclk   = true,
        .hw_gain    = { .pa_voltage = 3.3, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *spk_codec = es8311_codec_new(&es8311);
    ESP_RETURN_ON_FALSE(spk_codec, ESP_FAIL, TAG, "ES8311 not responding");
    esp_codec_dev_cfg_t spk_dev = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = spk_codec, .data_if = data_if };
    s_spk = esp_codec_dev_new(&spk_dev);
    ESP_RETURN_ON_FALSE(s_spk, ESP_FAIL, TAG, "speaker device");

    /* ---- ES7210 microphone ADC (I2C 0x40), all four inputs ---- */
    audio_codec_i2c_cfg_t mic_i2c = {
        .port = I2C_NUM_0, .addr = ES7210_CODEC_DEFAULT_ADDR, .bus_handle = board_i2c(),
    };
    const audio_codec_ctrl_if_t *mic_ctrl = audio_codec_new_i2c_ctrl(&mic_i2c);
    if (mic_ctrl) {
        es7210_codec_cfg_t es7210 = {
            .ctrl_if      = mic_ctrl,
            .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
        };
        const audio_codec_if_t *mic_codec = es7210_codec_new(&es7210);
        if (mic_codec) {
            esp_codec_dev_cfg_t mic_dev = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = mic_codec, .data_if = data_if };
            s_mic = esp_codec_dev_new(&mic_dev);
        }
    }
    if (!s_mic) {
        ESP_LOGW(TAG, "ES7210 microphone ADC not available");
    }

    s_rate = sample_rate;
    s_frame_samples = chan.dma_frame_num;
    s_frames = chan.dma_desc_num;
    ESP_LOGI(TAG, "ready: %u Hz, DMA %u x %u ms, speaker %s, microphones %s", (unsigned)sample_rate,
             (unsigned)frames, (unsigned)frame_ms, s_spk ? "ok" : "-", s_mic ? "ok (4 ch TDM)" : "-");
    return ESP_OK;
}

esp_codec_dev_handle_t audio_speaker(void)
{
    return s_spk;
}

esp_codec_dev_handle_t audio_mic(void)
{
    return s_mic;
}

const audio_codec_ctrl_if_t *audio_speaker_ctrl(void)
{
    return s_spk_ctrl;
}

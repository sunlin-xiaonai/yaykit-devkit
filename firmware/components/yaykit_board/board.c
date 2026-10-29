#include "board.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board_priv.h"
#include "yk_board_logic.h"

static const char *TAG = "board";

/* ---------------------------------------------------------------- display */

#define PIN_LCD_CS   12
#define PIN_LCD_CLK  38
#define PIN_LCD_D0   4
#define PIN_LCD_D1   5
#define PIN_LCD_D2   6
#define PIN_LCD_D3   7
#define PIN_LCD_RST  39
#define LCD_SPI_HOST SPI2_HOST
#define LCD_PCLK_HZ  (80 * 1000 * 1000)
#define LCD_X_GAP    6 /* the 466 px panel is centred in a 480 px controller RAM */

/* ------------------------------------------------------------------ touch */

#define PIN_TP_RST     40
#define TP_I2C_ADDR    0x15
#define TP_I2C_HZ      400000
#define TP_REG_FIRST   0x01 /* gesture, finger count, x hi/lo, y hi/lo */

static i2c_master_bus_handle_t s_i2c;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t    s_panel;
static i2c_master_dev_handle_t   s_touch;
static lv_display_t             *s_disp;
static lv_indev_t               *s_indev;
static int                       s_brightness = -1;
static int                       s_brightness_raw = -1;
static bool                      s_inited;
static uint8_t                   s_init_bright = 0xFF; /* the panel's brightness in its init sequence (0: start dark) */
static bool                      s_asleep;             /* board_display_sleep() */
static volatile bool             s_touch_asleep;       /* the screen is off: the touch controller is not read */

/* --------------------------------------------------------- power + i2c */

esp_err_t board_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    /* Peripheral power. Without this the I2C devices are unpowered and clamp
     * SDA/SCL low, which looks exactly like a missing pull-up. A hold from the
     * battery's deep sleep would keep it off whatever is set: let go first. */
    board_sleep_release();
    const gpio_config_t pwr = {
        .pin_bit_mask = 1ULL << BOARD_PIN_POWER_EN,
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pwr), TAG, "power pin");
    gpio_set_level(BOARD_PIN_POWER_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    const i2c_master_bus_config_t bus = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = BOARD_PIN_I2C_SDA,
        .scl_io_num        = BOARD_PIN_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true, /* the board has no external pull-ups */
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_i2c), TAG, "i2c bus");

    s_inited = true;
    ESP_LOGI(TAG, "power on, I2C ready (SDA=%d SCL=%d)", BOARD_PIN_I2C_SDA, BOARD_PIN_I2C_SCL);
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c(void)
{
    return s_i2c;
}

/* -------------------------------------------------------------- brightness */

/* SH8601 "write display brightness" (0x51) over QSPI: opcode 0x02 in the top byte. */
esp_err_t board_brightness_raw_set(uint8_t level)
{
    if (!s_io) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t cmd = (0x02u << 24) | (0x51u << 8);
    /* The panel IO is shared with the LVGL task's flushes and is not safe from two tasks at once: a tx_param racing
     * a draw_bitmap can wait for ever (Party Play 1.0.1 hung about once in 30 starts setting the first brightness
     * after board_unlock()). So it takes the board lock itself; the lock is recursive, so callers that hold it, the
     * LVGL task's own callbacks included, are fine. Before the display is registered there is no LVGL task to race,
     * and the lock may not exist yet (asking for it then only logs a false error). */
    const bool locked = s_disp && board_lock(-1) == ESP_OK;
    const esp_err_t err = esp_lcd_panel_io_tx_param(s_io, cmd, &level, 1);
    if (locked) {
        board_unlock();
    }
    if (err == ESP_OK) {
        s_brightness_raw = level;
        s_brightness     = (level * 100 + 127) / 255;
    }
    return err;
}

int board_brightness_raw_get(void)
{
    return s_brightness_raw;
}

esp_err_t board_brightness_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    const esp_err_t err = board_brightness_raw_set((uint8_t)(percent * 255 / 100));
    if (err == ESP_OK) {
        s_brightness = percent;
    }
    return err;
}

int board_brightness_get(void)
{
    return s_brightness;
}

/* ----------------------------------------------------------------- panel */

/* Init sequence of this panel, taken from the factory firmware (the controller
 * behaves like a CO5300: 0xFE switches register pages, 0xC4 = 0x80 turns on the
 * 4-wire data mode - without it the panel ignores QSPI pixel data and stays
 * black). The SH8601 driver's built-in default does NOT do this. */
static const sh8601_lcd_init_cmd_t s_init_cmds[] = {
    { 0xFE, (uint8_t[]){ 0x20 }, 1, 0 },
    { 0x19, (uint8_t[]){ 0x10 }, 1, 0 },
    { 0x1C, (uint8_t[]){ 0xA0 }, 1, 0 },
    { 0xFE, (uint8_t[]){ 0x00 }, 1, 0 },
    { 0xC4, (uint8_t[]){ 0x80 }, 1, 0 },
    { 0x3A, (uint8_t[]){ 0x55 }, 1, 0 }, /* 16 bit / pixel */
    { 0x35, (uint8_t[]){ 0x00 }, 1, 0 }, /* tearing effect line on */
    { 0x53, (uint8_t[]){ 0x20 }, 1, 0 }, /* enable brightness control */
    { 0x51, &s_init_bright, 1, 0 },      /* brightness: 0xFF, or 0 to start dark */
    { 0x63, (uint8_t[]){ 0xFF }, 1, 0 }, /* HBM brightness */
    { 0x2A, (uint8_t[]){ 0x00, 0x06, 0x01, 0xD7 }, 4, 0 },
    { 0x2B, (uint8_t[]){ 0x00, 0x00, 0x01, 0xD1 }, 4, 0 },
    { 0x11, NULL, 0, 120 }, /* sleep out */
    { 0x29, NULL, 0, 20 },  /* display on */
};

static esp_err_t panel_create(void)
{
    const spi_bus_config_t bus = SH8601_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_CLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
        BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = SH8601_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, NULL, NULL);
    io_cfg.pclk_hz = LCD_PCLK_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io),
                        TAG, "panel io");

    sh8601_vendor_config_t vendor = {
        .init_cmds      = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags          = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel), TAG, "panel");

    esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, 0);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel on");
    return ESP_OK;
}

/* The controller wants every update to start on an even and end on an odd
 * pixel, in both directions. */
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
    yk_board_round_area(&a->x1, &a->y1, &a->x2, &a->y2);
}

/* ----------------------------------------------------------------- touch */

static esp_err_t touch_create(void)
{
    /* hardware reset, active low */
    const gpio_config_t rst = {
        .pin_bit_mask = 1ULL << PIN_TP_RST,
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst), TAG, "tp rst");
    gpio_set_level(PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = TP_I2C_ADDR,
        .scl_speed_hz    = TP_I2C_HZ,
    };
    return i2c_master_bus_add_device(s_i2c, &dev, &s_touch);
}

esp_err_t board_touch_read(uint8_t report[6])
{
    if (!s_touch) {
        return ESP_ERR_INVALID_STATE;
    }
    /* the screen is off: touches are not taken, and the bus is left to the others */
    if (s_touch_asleep) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t reg = TP_REG_FIRST;
    return i2c_master_transmit_receive(s_touch, &reg, 1, report, 6, 10);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static lv_point_t last;

    bool pressed;
    int32_t x, y;
    if (board_touch_fast_point(&pressed, &x, &y)) {
        /* the fast reader has the controller: LVGL takes its latest read, no second I2C read */
        if (pressed) last.x = x, last.y = y;
        data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->point = last;
        return;
    }
    uint8_t b[6] = { 0 };
    const esp_err_t err = board_touch_read(b);

    if (err == ESP_OK && yk_board_touch_point(b, BOARD_LCD_H_RES, BOARD_LCD_V_RES, &last.x, &last.y)) {
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point = last;
}

/* ------------------------------------------------------------ screen off (P22) */

/* The touch controller is not put into deep sleep: leaving it takes a reset, and on the device that reset (at the
 * key's press) broke the ES7210's I2C writes as the microphones opened, on the same bus, 3 times in 5 (once the
 * microphones did not open). Unread, the CST820 goes to its own low-power scanning when nothing touches it. */

esp_err_t board_display_sleep(void)
{
    if (!s_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_asleep) {
        return ESP_OK;
    }
    s_asleep = true;
    esp_err_t err = board_brightness_raw_set(0);
    const esp_err_t off = esp_lcd_panel_disp_on_off(s_panel, false); /* 0x28 */
    if (err == ESP_OK) err = off;
    if (s_disp) {
        lv_display_enable_invalidation(s_disp, false); /* a counter: board_display_wake() gives it back */
    }
    s_touch_asleep = true;
    ESP_LOGD(TAG, "screen off");
    return err;
}

esp_err_t board_sleep_prepare(bool panel)
{
    esp_err_t err = ESP_OK;
    if (panel && s_panel) {
        err = board_display_sleep();
        const esp_err_t in = esp_lcd_panel_io_tx_param(s_io, 0x10, NULL, 0); /* sleep in */
        if (err == ESP_OK) err = in;
        vTaskDelay(pdMS_TO_TICKS(10)); /* the SH8601 wants 5 ms after sleep-in before its supply may go */
    }
    gpio_set_level(BOARD_PIN_POWER_EN, 0); /* an output since board_init(); else its pull-down holds it off anyway */
    const esp_err_t held = gpio_hold_en(BOARD_PIN_POWER_EN);
    return err == ESP_OK ? held : err;
}

void board_sleep_release(void)
{
    gpio_hold_dis(BOARD_PIN_POWER_EN);
}

esp_err_t board_display_wake(int percent)
{
    if (!s_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_asleep) {
        return ESP_OK;
    }
    s_asleep = false;
    esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, true); /* 0x29, still at brightness 0 */
    const esp_err_t bright = board_brightness_set(percent < 0 ? board_pref_brightness() : percent);
    if (err == ESP_OK) err = bright;
    if (s_disp) {
        lv_display_enable_invalidation(s_disp, true);
        lv_obj_invalidate(lv_display_get_screen_active(s_disp)); /* nothing was drawn while it slept */
    }
    s_touch_asleep = false;
    ESP_LOGD(TAG, "screen on");
    return err;
}

bool board_display_asleep(void)
{
    return s_asleep;
}

/* ------------------------------------------------------------------ LVGL */

lv_display_t *board_display_start(void)
{
    return board_display_start_opts(NULL);
}

lv_display_t *board_display_start_opts(const board_display_opts_t *opts)
{
    const bool dark = opts && opts->start_dark;
    s_init_bright = dark ? 0x00 : 0xFF;
    if (board_init() != ESP_OK) {
        return NULL;
    }
    if (panel_create() != ESP_OK) {
        return NULL;
    }
    if (touch_create() != ESP_OK) {
        ESP_LOGW(TAG, "touch controller not found, running without touch");
    }

    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    cfg.task_core_id = CONFIG_YK_BOARD_LVGL_CORE;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&cfg));

    const esp_lv_adapter_display_config_t disp_cfg = {
        .panel = s_panel,
        .panel_io = s_io,
        .profile = {
            .interface             = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation              = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res               = BOARD_LCD_H_RES,
            .ver_res               = BOARD_LCD_V_RES,
            /* One band (40 lines = 37 KB by default, CONFIG_YK_BOARD_LCD_BUFFER_LINES) in internal, DMA-capable RAM. From PSRAM
             * the SPI driver has to malloc an internal bounce copy of every band
             * at flush time, which fails once Wi-Fi and BLE have fragmented the
             * heap; DMA straight from PSRAM (psram_dma_direct) loses data here
             * (spike S1). Single buffer: sending a band takes ~1 ms, rendering
             * it far longer, so a second buffer would buy nothing. Even height
             * keeps the panel's even-start rule. */
            .buffer_height         = CONFIG_YK_BOARD_LCD_BUFFER_LINES,
            .use_psram             = false,
            .enable_ppa_accel      = false,
            .require_double_buffer = false,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
    };
    s_disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!s_disp) {
        return NULL;
    }
    lv_display_add_event_cb(s_disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    if (s_touch) {
        board_lock(-1);
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touch_read_cb);
        lv_indev_set_display(s_indev, s_disp);
        board_unlock();
    }

    if (dark) {
        s_brightness_raw = 0;
        s_brightness     = 0;
    } else {
        board_brightness_set(100);
    }
    ESP_LOGI(TAG, "display %dx%d ready, touch %s%s", BOARD_LCD_H_RES, BOARD_LCD_V_RES,
             s_indev ? "on" : "off", dark ? ", dark until the first frame" : "");
    return s_disp;
}

esp_err_t board_lock(int32_t timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms);
}

void board_unlock(void)
{
    esp_lv_adapter_unlock();
}

lv_indev_t *board_touch_indev(void)
{
    return s_indev;
}

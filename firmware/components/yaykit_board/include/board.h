/*
 * Board support for the 1.75" round AMOLED ESP32-S3 device.
 *
 * Everything here was measured on the actual hardware (live GPIO matrix of the
 * working factory image, an I2C scan, and the factory init code), not taken
 * from a reference-board datasheet - the pin map is *almost* but not exactly
 * the Waveshare board. In particular:
 *
 *   GPIO21   peripheral power enable; must be HIGH before anything on the
 *            I2C bus exists (with it low SDA/SCL are clamped low)
 *   I2C0     SDA=15 SCL=14, internal pull-ups only (no external ones)
 *              0x15 CST820 touch      0x18 ES8311 codec     0x40 ES7210 mic ADC
 *              0x51 PCF85063 RTC      0x6B QMI8658 IMU      0x3C/0x7E power/battery
 *   Display  SH8601 (not CO5300), QSPI on SPI2: D0..D3 = 4,5,6,7  CLK=38  CS=12
 *            RST=39, 80 MHz, 466x466 RGB565, 6 px horizontal gap
 *   Touch    CST820, RST=40; INT is TP_INT on the reference schematic (docs/hardware/ESP32-S3-1.75-AMOLED-AI.pdf),
 *            GPIO11 or GPIO13 there; CONFIG_YK_BOARD_TOUCH_INT_GPIO says which once measured (-1: polled)
 *   Battery  V_BAT / 2 (100k + 100k) on GPIO17 (ADC2 ch 6); USB 5 V / 2 (200k + 200k) on GPIO18 (ADC2 ch 7), same
 *            schematic; the SW6106 power chip (I2C 0x3C) is read-only
 *   Audio    I2S0: MCLK=42 BCLK=9 WS=45 DOUT=8 DIN=10, amplifier enable=43 (high = on)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "lvgl.h"

#define BOARD_LCD_H_RES 466
#define BOARD_LCD_V_RES 466

#define BOARD_PIN_POWER_EN 21
#define BOARD_PIN_I2C_SDA  15
#define BOARD_PIN_I2C_SCL  14

#define BOARD_PIN_I2S_MCLK 42
#define BOARD_PIN_I2S_BCLK 9
#define BOARD_PIN_I2S_WS   45
#define BOARD_PIN_I2S_DOUT 8
#define BOARD_PIN_I2S_DIN  10
#define BOARD_PIN_PA_EN    43 /* speaker amplifier: yaykit_speak's; esp_codec_dev does not switch it */
#define BOARD_PIN_VBAT     17 /* battery voltage / 2 (ADC2 channel 6) */
#define BOARD_PIN_VBUS     18 /* USB 5 V / 2 (ADC2 channel 7): external power present */

/* Powers the peripherals and creates the shared I2C bus. Idempotent. */
esp_err_t board_init(void);

/* Shared I2C bus (I2C_NUM_0). Valid after board_init(). */
i2c_master_bus_handle_t board_i2c(void);

/* Display + touch + LVGL task. Returns the LVGL display or NULL. The panel comes up at 100 %. */
lv_display_t *board_display_start(void);

typedef struct {
    /* The panel comes up at brightness 0 and stays dark until board_brightness_set() / _raw_set() (P10): no flash of
     * 100 % before the app's first frame (at night, after a switch from Yay). Draw the first frame, lv_refr_now(),
     * then set the brightness: board_pref_brightness() unless the app has its own rule. */
    bool start_dark;
} board_display_opts_t;
/* board_display_start() with options; NULL opts = board_display_start(). */
lv_display_t *board_display_start_opts(const board_display_opts_t *opts);

/* LVGL is not thread safe: take this around any lv_* call made outside an LVGL
 * callback. timeout_ms = -1 waits forever. */
esp_err_t board_lock(int32_t timeout_ms);
void      board_unlock(void);

lv_indev_t *board_touch_indev(void);

/* The battery (P20). The voltage comes from the ESP32's own ADC on V_BAT / 2 (GPIO17), which reads on battery and
 * on USB alike; the SW6106's registers 0x14/0x15 (read-only over I2C, nothing is ever written to that chip: it is
 * the power path) read 0 on battery and are only a cross-check now. mv averaged over a few reads; percent from the
 * resting cell's curve, corrected for the rise while charging (yk_board_logic.h), -1 when unknown. usb: external
 * power is present (USB 5 V / 2 on GPIO18). charging: on USB and not yet full. `status` is SW6106 register 0x06
 * as read (-1 when that read failed), for diagnostics. ESP_ERR_NOT_FOUND when no voltage could be read at all. */
typedef struct {
    int mv;
    int percent;
    int status;
    bool usb;       /* external power present */
    bool charging;  /* on USB and the cell is not full yet */
} board_battery_t;
esp_err_t board_battery(board_battery_t *out);
/* Development: the SW6106's registers 0x00..0x3F as read (still nothing written), to find what plugging in
 * changes. A register that did not answer reads 0xFF. */
esp_err_t board_battery_regs(uint8_t regs[64]);

/* AMOLED brightness, 0..100, set through the panel controller. Takes the board lock itself (recursive): safe from any
 * task, with or without the lock held, the LVGL task's callbacks included. */
esp_err_t board_brightness_set(int percent);
int       board_brightness_get(void);
/* The panel's own scale, 0..255 (P10): finer steps at the dark end than percent (Breathe's night scenes go to a few
 * units). board_brightness_get() then reports the nearest percent. */
esp_err_t board_brightness_raw_set(uint8_t level);
int       board_brightness_raw_get(void); /* -1 before the display started */

/* The screen off and on again (P22), under board_lock() or in an LVGL callback; each is a no-op when it is so already.
 * board_display_sleep(): brightness 0 and the SH8601's display off (an AMOLED then shows nothing at all), the CST820
 * no longer read (touches do nothing; it is not put into deep sleep, whose reset on waking broke the microphones'
 * I2C on the same bus), and LVGL stops drawing (objects still change; nothing is sent).
 * board_display_wake(percent): the display on at `percent` (-1: board_pref_brightness(); 0 to fade it in with
 * board_brightness_set()), the next frame drawn whole, touches read again at once. Only the side key should wake a
 * sleeping screen: touch cannot. */
esp_err_t board_display_sleep(void);
esp_err_t board_display_wake(int percent);
/* Deep sleep (the battery's, P25), called last before esp_deep_sleep_start(): with `panel`, the SH8601 goes from
 * display off into sleep-in (0x10; the caller holds board_lock(), the panel shares the SPI bus with drawing); then
 * the peripheral power (GPIO21) goes off and is held off through the sleep. I2C and the panel are gone after it.
 * GPIO21 is an RTC pad: the hold outlives the wake and a software restart, so board_sleep_release() lets it go. */
esp_err_t board_sleep_prepare(bool panel);
/* The hold board_sleep_prepare() left on GPIO21, released (nothing when there is none). board_init() calls it; Yay
 * calls it first thing at start, before anything may restart into firmware that does not know of the hold. */
void board_sleep_release(void);
bool      board_display_asleep(void);

/* ---------------------------------------------------------------- shared preferences (P10)
 * Volume, brightness and the screen-off time are the device's, not an app's: NVS namespace "yk_pref", shared by
 * every firmware (main, apps, recovery). Set in Yay, every app opens with the same values; an app that changes
 * one (its own volume dial) writes it back. NVS must be initialised. Reads never fail: a missing or bad value
 * reads as the default (the first read also takes the volume the main firmware kept before, namespace `yay`). */
#define BOARD_PREF_VOLUME_DEFAULT     6   /* 0..10, yaykit_speak's steps */
#define BOARD_PREF_BRIGHTNESS_DEFAULT 80  /* percent, 5..100 */
#define BOARD_PREF_SCREEN_OFF_DEFAULT 30  /* seconds of no input before the screen goes off; 0 = never (180 before P22) */
int       board_pref_volume(void);
esp_err_t board_pref_set_volume(int level);         /* clamped to 0..10 */
int       board_pref_brightness(void);
esp_err_t board_pref_set_brightness(int percent);   /* clamped to 5..100 (0 would look like a dead device) */
int       board_pref_screen_off_s(void);
esp_err_t board_pref_set_screen_off_s(int seconds); /* 0 or 15..3600 */

/* ---------------------------------------------------------------- fast touch (P20)
 * LVGL reads the touch controller once a frame (about every 33 ms). For a reaction game, board_touch_fast_start()
 * reads it every `period_ms` (5 for Party Play's snap game) on its own task, or at the controller's interrupt when
 * the pin is wired (CONFIG_YK_BOARD_TOUCH_INT_GPIO >= 0), and timestamps each change with esp_timer. LVGL keeps
 * working meanwhile from the same reads (no second I2C read per frame). Stop it when the timed part is over: it
 * costs a read on the shared I2C bus every period. */
typedef struct {
    int64_t t_us;  /* esp_timer time of the read (or interrupt) that saw the change */
    int16_t x, y;  /* screen pixels; the last point for a release */
    bool pressed;  /* finger down (true) or up (false) */
} board_touch_edge_t;
esp_err_t board_touch_fast_start(int period_ms);  /* 2..20 ms; ESP_ERR_INVALID_STATE without touch */
void      board_touch_fast_stop(void);
/* Next press or release since the start, oldest first (a queue of 16; the oldest is dropped when full). */
bool      board_touch_fast_take(board_touch_edge_t *out);
/* Drops what is queued (at the start of a round). */
void      board_touch_fast_clear(void);
/* The interrupt pin is in use (else polled). */
bool      board_touch_fast_irq(void);

/* The real-time clock (PCF85063A): the time survives a restart without the
 * network, so recordings made offline right after boot have their real start
 * time. SNTP stays the source of truth; every sync is written to the chip. */

/* Sets the system time from the chip if it holds a trustworthy time. Call
 * once at boot, after the display (which starts the I2C bus). */
void board_rtc_init(void);
/* Writes the system time to the chip and starts it (after an SNTP sync). */
void board_rtc_save(void);
/* Did board_rtc_init() find a usable time? */
bool board_rtc_restored(void);

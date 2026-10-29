// Pure C parts of yaykit_board, host tested with test/run.sh (no ESP-IDF needed).
#pragma once

#include <stdbool.h>
#include <stdint.h>

// The panel controller wants every update to start on an even and end on an odd pixel, in both directions.
void yk_board_round_area(int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2);

// Reads a CST820 report (registers 0x01..0x06: gesture, finger count, x hi/lo, y hi/lo). With a finger down it
// writes the point, clamped to width x height, and returns true; otherwise it leaves the point and returns false.
bool yk_board_touch_point(const uint8_t report[6], int32_t width, int32_t height, int32_t *x, int32_t *y);

// Sample rates the shared I2S port runs at. Both directions use MCLK = 256 x fs, which ES8311 and ES7210
// take at 16 kHz (speech, ESP-SR) and 24 kHz (the default the current apps were tuned at).
bool yk_board_audio_rate_ok(uint32_t hz);

// The battery (SW6106, I2C 0x3C, read-only): registers 0x14 (low byte) and 0x15 (low nibble = the top 4 bits) are a
// 12-bit ADC value at 1.2 mV a step (tikpal-firmware docs/硬件信息参考.md 5.8, checked against the factory firmware's
// own log: raw 3344 = 4012.8 mV).
int yk_board_battery_mv(uint8_t reg14, uint8_t reg15);
// A rough level from the voltage, 0..100: a resting Li-ion cell's curve (3.3 V empty, 4.2 V full). Rough on purpose:
// the voltage reads high while charging, and the factory firmware's own formula is not known.
int yk_board_battery_percent(int mv);
// Whether a voltage from those registers is the battery's: they read 0 while the device runs on its battery (first
// end-to-end test, 2026-10-19: 0 mV unplugged, 4.39–4.43 V on USB), so the level is known only on USB for now.
bool yk_board_battery_known(int mv);

// The battery from the ESP32's ADC (P20): V_BAT through 100k + 100k on GPIO17, USB 5 V through 200k + 200k on GPIO18
// (reference schematic). The ADC reads millivolts at the pin (calibrated, 12 dB attenuation: up to ~3.1 V).
int yk_board_vbat_mv(int pin_mv);
// External power: the USB pin reads about 2.5 V with 5 V present, ~0 without (threshold 1.6 V = 3.2 V on the bus).
bool yk_board_vbus_present(int pin_mv);
// The level while charging: the cell reads higher under charge current, so the rise is taken off (YK_BOARD_CHARGE_RISE_MV)
// and the level stays below 100 until the charger stops (full: no rise, the resting curve says 100).
#define YK_BOARD_CHARGE_RISE_MV 120
int yk_board_battery_level(int mv, bool charging);
// Full while on USB: the cell at the charger's end voltage (4.15 V and up).
bool yk_board_battery_full(int mv);

// The shared preferences (P10): the valid range of each, a bad stored value reading as the default.
typedef enum { YK_PREF_VOLUME, YK_PREF_BRIGHTNESS, YK_PREF_SCREEN_OFF } yk_pref_t;
int yk_board_pref_clamp(yk_pref_t which, int value);
int yk_board_pref_default(yk_pref_t which);
// A value read back from NVS (or -1 when missing): in range it stands, out of range it is the default.
int yk_board_pref_read(yk_pref_t which, int stored);

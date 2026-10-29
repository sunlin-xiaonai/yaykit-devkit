// The battery (P20). The cell's voltage from the ESP32's own ADC on V_BAT / 2 (GPIO17), which reads on battery as on
// USB, and external power from USB 5 V / 2 (GPIO18), per the reference schematic. The SW6106 power chip (I2C 0x3C)
// is read only, for its status register and as a cross-check: its voltage registers read 0 on battery. Nothing is
// ever written to it: it is the power path.
#include "board.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "yk_board_logic.h"

static const char *TAG = "board_power";

#define SW6106_ADDR 0x3C
#define VBAT_CH ADC_CHANNEL_6  // GPIO17 on ADC2
#define VBUS_CH ADC_CHANNEL_7  // GPIO18 on ADC2

static i2c_master_dev_handle_t s_power;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali_bat, s_cali_bus;
static SemaphoreHandle_t s_lock;

static esp_err_t power_dev(void)
{
    if (!board_i2c()) return ESP_ERR_INVALID_STATE;
    if (s_power) return ESP_OK;
    const i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = SW6106_ADDR,
                                     .scl_speed_hz = 100000};
    return i2c_master_bus_add_device(board_i2c(), &cfg, &s_power);
}

/* One register, read: the address goes out, the value comes back (nothing is written into the chip). */
static esp_err_t power_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_power, &reg, 1, value, 1, 20);
}

esp_err_t board_battery_regs(uint8_t regs[64])
{
    ESP_RETURN_ON_ERROR(power_dev(), TAG, "battery");
    int ok = 0;
    for (int r = 0; r < 64; r++) {
        regs[r] = 0xFF;
        for (int i = 0; i < 3; i++)  // the touch controller shares the bus
            if (power_read((uint8_t)r, &regs[r]) == ESP_OK) {
                ok++;
                break;
            }
    }
    return ok ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static adc_cali_handle_t cali(adc_channel_t ch)
{
    adc_cali_handle_t h = NULL;
    const adc_cali_curve_fitting_config_t cfg = {
        .unit_id = ADC_UNIT_2, .chan = ch, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&cfg, &h) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC calibration for channel %d: raw scaling", ch);
        return NULL;
    }
    return h;
}

static esp_err_t adc_start(void)
{
    if (s_adc) return ESP_OK;
    if (!s_lock && !(s_lock = xSemaphoreCreateMutex())) return ESP_ERR_NO_MEM;
    const adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_2, .ulp_mode = ADC_ULP_MODE_DISABLE};
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit, &s_adc), TAG, "ADC2");
    const adc_oneshot_chan_cfg_t ch = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, VBAT_CH, &ch), TAG, "VBAT");
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, VBUS_CH, &ch), TAG, "VBUS");
    s_cali_bat = cali(VBAT_CH);
    s_cali_bus = cali(VBUS_CH);
    return ESP_OK;
}

/** Millivolts at the pin, averaged over n reads; -1 when none worked (Wi-Fi holds ADC2 now and then). */
static int pin_mv(adc_channel_t ch, adc_cali_handle_t cal, int n)
{
    int sum = 0, ok = 0;
    for (int i = 0; i < n * 3 && ok < n; i++) {
        int raw = 0, mv = 0;
        if (adc_oneshot_read(s_adc, ch, &raw) != ESP_OK) {
            vTaskDelay(1);
            continue;
        }
        if (!cal || adc_cali_raw_to_voltage(cal, raw, &mv) != ESP_OK) mv = raw * 3100 / 4095;
        sum += mv;
        ok++;
    }
    return ok ? sum / ok : -1;
}

esp_err_t board_battery(board_battery_t *out)
{
    *out = (board_battery_t){.mv = 0, .percent = -1, .status = -1};
    esp_err_t err = adc_start();
    int bat = -1, bus = -1;
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bat = pin_mv(VBAT_CH, s_cali_bat, 8);
        bus = pin_mv(VBUS_CH, s_cali_bus, 4);
        xSemaphoreGive(s_lock);
    } else {
        ESP_LOGW(TAG, "ADC: %s", esp_err_to_name(err));
    }
    if (power_dev() == ESP_OK) {
        uint8_t status = 0;
        out->status = power_read(0x06, &status) == ESP_OK ? status : -1;
    }
    if (bat < 0) return ESP_ERR_NOT_FOUND;
    out->mv = yk_board_vbat_mv(bat);
    out->usb = bus >= 0 && yk_board_vbus_present(bus);
    out->charging = out->usb && !yk_board_battery_full(out->mv);
    out->percent = yk_board_battery_level(out->mv, out->charging);
    return yk_board_battery_known(out->mv) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

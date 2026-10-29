#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "board.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "rtc_regs.h"

static const char *TAG = "clock";

#define RTC_ADDR 0x51

static i2c_master_dev_handle_t s_dev;
static bool                    s_restored;

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 50);
}

static esp_err_t wr(uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t b[1 + RTC_TIME_REGS];
    b[0] = reg;
    memcpy(b + 1, buf, n);
    return i2c_master_transmit(s_dev, b, n + 1, 50);
}

void board_rtc_init(void)
{
    const i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = RTC_ADDR, .scl_speed_hz = 100000 };
    if (i2c_master_bus_add_device(board_i2c(), &cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return;
    }
    uint8_t ctl = 0, r[RTC_TIME_REGS];
    int64_t t   = 0;
    if (rd(RTC_REG_CONTROL1, &ctl, 1) != ESP_OK || rd(RTC_REG_SECONDS, r, sizeof(r)) != ESP_OK) {
        ESP_LOGW(TAG, "clock chip not answering");
        return;
    }
    if ((ctl & RTC_STOP) || !rtc_regs_to_unix(r, &t)) {
        ESP_LOGI(TAG, "no stored time (%s): waiting for the network", ctl & RTC_STOP ? "stopped" : "never set or lost");
        return;
    }
    const struct timeval tv = { .tv_sec = (time_t)t };
    settimeofday(&tv, NULL);
    s_restored = true;
    ESP_LOGI(TAG, "time restored from the clock chip: %lld", (long long)t);
}

void board_rtc_save(void)
{
    if (!s_dev) return;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint8_t r[RTC_TIME_REGS], ctl = 0;
    if (rd(RTC_REG_CONTROL1, &ctl, 1) != ESP_OK) return;
    rtc_regs_from_unix((int64_t)tv.tv_sec, r);
    /* halt, set, run: the chip restarts its second from 0 when STOP is cleared */
    const uint8_t halted = ctl | RTC_STOP, running = ctl & (uint8_t)~RTC_STOP;
    if (wr(RTC_REG_CONTROL1, &halted, 1) != ESP_OK || wr(RTC_REG_SECONDS, r, sizeof(r)) != ESP_OK ||
        wr(RTC_REG_CONTROL1, &running, 1) != ESP_OK) {
        ESP_LOGW(TAG, "could not set the clock chip");
        return;
    }
    ESP_LOGI(TAG, "clock chip set");
}

bool board_rtc_restored(void)
{
    return s_restored;
}

# Board support for tikpal-r1, the only place that knows the hardware: power rails, I2C, the SH8601 panel and
# CST820 touch with the LVGL glue, the PCF85063A clock, ES8311/ES7210 audio on one I2S port (16 or 24 kHz) and
# the shared Chinese font; the shared preferences (P10), the battery by ADC and fast touch (P20); drawing straight to
# the panel around LVGL (board_direct.h, the selfie viewfinder). Moved from the
# Tikpal firmware; README.md says from where and what changed.
idf_component_register(
    SRCS "board.c" "board_rtc.c" "rtc_regs.c" "lvgl_mem_psram.c" "audio.c" "cjk_font.c" "yk_font_pack.c" "yk_board_logic.c"
         "board_pref.c" "board_power.c" "board_touch_fast.c" "board_direct.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c lvgl__lvgl espressif__esp_codec_dev
    PRIV_REQUIRES
        esp_adc
        esp_driver_gpio
        esp_timer
        nvs_flash
        esp_driver_i2s
        esp_driver_spi
        esp_lcd
        esp_partition
        esp_psram
        espressif__esp_lvgl_adapter
        waveshare__esp_lcd_sh8601
)
# lvgl_mem_psram.c defines functions LVGL calls (lv_malloc_core ...), but LVGL links after this library;
# asking for one of them keeps that object (Tikpal kept the whole archive, which would also pull in audio).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")

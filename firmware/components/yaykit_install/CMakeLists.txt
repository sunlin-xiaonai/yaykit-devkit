# Installs app images into slots (docs/design/yay-device.md 6.8): HTTPS with resume, the package cache (pkgcache) and
# USB; checks size, SHA-256, signature (yaykit_sig) and project name. The pure parts are host tested (test/run.sh).
idf_component_register(SRCS "yaykit_install.c" "yk_install_logic.c" "yk_install_keys.c"
    INCLUDE_DIRS "include"
    REQUIRES json esp_common
    PRIV_REQUIRES yaykit_sig yaykit_app yaykit_link yaykit_wifi app_update bootloader_support esp_partition spi_flash
                  esp_http_client mbedtls nvs_flash console esp_timer esp_driver_usb_serial_jtag)

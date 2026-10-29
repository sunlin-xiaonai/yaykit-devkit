set(srcs "yaykit_link.c" "yaykit_slots.c" "yk_line.c" "yk_srp_salt.c" "yk_mem.c" "yk_link_bond.c")
# The long mode (yk_link_keep.h) talks to NimBLE itself: only firmware with NimBLE on builds it (Party Play has no
# Bluetooth at all).
if(CONFIG_BT_NIMBLE_ENABLED)
    list(APPEND srcs "yk_link_keep.c")
endif()
idf_component_register(SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES json
    PRIV_REQUIRES app_update protocomm wifi_provisioning bt console esp_timer esp_event mbedtls esp_psram spi_flash esp_hw_support
                  heap nvs_flash)

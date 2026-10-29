# Shared device UI for Yaykit firmware on the Tikpal board: labels and buttons in the
# Yaykit style, the "Connect phone" pairing page and the `snap` frame capture command; v2 (P11): the round-screen
# spec's rim, Yay, notices, transitions and subtitles (pure parts in yk_ui_logic.c, host tested).
# Yay's art (assets/yay) is embedded here; only firmware that draws Yay (yk_yay) pulls it in at link time. Likewise the
# pairing page (yk_ui_pair.c) is the only part that uses Yaykit Link's Bluetooth and LVGL's QR code: firmware without
# Bluetooth (Party Play, Breathe) can use the rest.
# P23/P24: the app ring (yk_apps), the app catalog cache it reads (yk_catalog, in `userfs`) and the hint arrow
# (yk_hint, counts in NVS `yk_hint`); pure parts in yk_apps_logic.c, host tested. App ecosystem stage 1: 「我的应用」
# (yk_more, Yay's page behind the ring's 「更多」); pure parts in yk_more_logic.c, host tested.
include(${CMAKE_CURRENT_LIST_DIR}/../../assets/yay/yay_art.cmake)
idf_component_register(SRCS "yaykit_ui.c" "yk_ui_pair.c" "yk_ui_logic.c" "yk_rim.c" "yk_yay.c" "yk_notice.c" "yk_caption.c"
                            "yk_transition.c" "yk_apps_logic.c" "yk_apps.c" "yk_catalog.c" "yk_hint.c" "yk_more_logic.c" "yk_more.c"
                            ${YAY_ART_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS ${YAY_ART_INCLUDE_DIRS}
    EMBED_FILES ${YAY_ART_EMBED_FILES}
    REQUIRES lvgl__lvgl
    PRIV_REQUIRES yaykit_board yaykit_link console mbedtls esp_psram esp_partition esp_timer nvs_flash)

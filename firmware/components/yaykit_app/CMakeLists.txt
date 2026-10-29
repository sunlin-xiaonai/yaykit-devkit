# The life of every Yay firmware: banner, image confirmation, launch parameters and results, going home,
# recovery, `app` and (development builds) `yay` console commands. Pure parts host tested (test/run.sh).
# P23: in an extension the double click opens yaykit_ui's app ring (yk_app_ring.c), and a hold the app did not use
# asks 「回到 Yay 说？」.
# P2: the recovery's version from its ELF sha256 (yk_recovery_logic.c, pure) and, for one not in the table, its image's
# sha256 (yk_recovery.c).
idf_component_register(SRCS "yaykit_app.c" "yk_app_logic.c" "yk_app_ring.c" "yk_recovery_logic.c" "yk_recovery.c"
    INCLUDE_DIRS "include"
    REQUIRES json esp_common yaykit_key
    PRIV_REQUIRES yaykit_link yaykit_ui yaykit_board lvgl__lvgl nvs_flash app_update console esp_system esp_timer
                  esp_psram bootloader_support esp_partition mbedtls)

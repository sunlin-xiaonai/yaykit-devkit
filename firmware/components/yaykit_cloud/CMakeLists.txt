idf_component_register(SRCS "yaykit_account.c" "yaykit_cloud.c" "yk_http.c" "yk_codec.c" "yk_sse.c" "yk_sign.c" "yk_api.c" "yk_locale.c"
    INCLUDE_DIRS "include"
    REQUIRES json esp_http_client
    PRIV_REQUIRES mbedtls nvs_flash esp_timer console esp_hw_support esp_psram yaykit_link yaykit_wifi)

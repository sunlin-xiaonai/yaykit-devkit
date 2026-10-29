idf_component_register(SRCS "yaykit_wifi.c" "yk_wifi_creds.c" "yk_wifi_ap.c"
    INCLUDE_DIRS "include"
    REQUIRES json wifi_provisioning esp_wifi
    PRIV_REQUIRES esp_netif esp_event esp_timer console nvs_flash)

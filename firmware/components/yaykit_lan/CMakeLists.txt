# One WebSocket client on the round screen's own network (自拍屏's viewfinder, docs/design/selfie.md 3). The pure parts
# (the token, hello) are in yk_lan_logic.c, host tested with test/run.sh.
idf_component_register(SRCS "yaykit_lan.c" "yk_lan_logic.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES esp_http_server json lwip esp_hw_support heap)

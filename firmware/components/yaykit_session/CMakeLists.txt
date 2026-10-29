# The main firmware's session with the cloud (docs/design/yay-device.md 4): WebSocket over TLS, reconnects, and in
# development builds a USB bridge. yk_proto (the messages and frames) is pure C, host tested against the
# platform's fixtures (test/run.sh).
idf_component_register(SRCS "yaykit_session.c" "yk_proto.c"
    INCLUDE_DIRS "include" "."
    REQUIRES json esp_common
    PRIV_REQUIRES espressif__esp_websocket_client esp-tls mbedtls console esp_timer)

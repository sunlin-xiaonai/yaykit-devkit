# The side key (GPIO0): debounced events from one task; the state machine is pure C (test/run.sh).
idf_component_register(SRCS "yaykit_key.c" "yk_key_sm.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_gpio esp_timer)

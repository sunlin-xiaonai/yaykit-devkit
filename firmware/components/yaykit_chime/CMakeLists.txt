# Short synthesized chimes through the ES8311 and the speaker (Homework Buddy's focus timer). Moved unchanged
# from the Tikpal firmware; README.md says from where. yaykit_speak's yk_speak_chime() will replace it.
idf_component_register(
    SRCS "chime.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES yaykit_board esp_driver_gpio esp_psram
)

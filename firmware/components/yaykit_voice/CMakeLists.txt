# Uses the board's audio (yaykit_board); add its directory to EXTRA_COMPONENT_DIRS.
idf_component_register(SRCS "yaykit_voice.c" "yk_wav.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES yaykit_board espressif__esp_codec_dev esp_psram)

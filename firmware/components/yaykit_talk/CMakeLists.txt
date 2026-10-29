# The voice session for extension firmwares (extension-platform.md P1, docs/delivery/2026-10-24-yaykit-talk.md): the
# main firmware's push-to-talk conversation over yaykit_session, yaykit_audio and yaykit_speak, and the device's
# settings by voice. The plain C parts (yk_talk_sm, yk_talk_setting, yk_talk_hello, yk_talk_msg, yk_talk_flush,
# yk_talk_close, yk_talk_setup) are host tested with test/run.sh.
idf_component_register(SRCS "yaykit_talk.c" "yk_talk_mic.c" "yk_talk_play.c" "yk_talk_sm.c" "yk_talk_setting.c"
                            "yk_talk_hello.c" "yk_talk_msg.c" "yk_talk_flush.c"
                            "yk_talk_close.c" "yk_talk_setup.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES yaykit_session yaykit_key esp_common
    PRIV_REQUIRES yaykit_audio yaykit_speak yaykit_board yaykit_cloud yaykit_app yaykit_link json console esp_timer
                  esp_wifi)

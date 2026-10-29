# The device's posture and movement from the QMI8658 (extension platform P12): events, gravity and linear
# acceleration in screen coordinates, the holder's direction, the shared calibration. The pure core
# (yk_motion_core.c) is host tested with test/run.sh; yaykit_motion.c is the device side (sensor task, NVS, console).
idf_component_register(SRCS "yk_motion_core.c" "yaykit_motion.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES yaykit_board esp_driver_i2c esp_timer nvs_flash console)

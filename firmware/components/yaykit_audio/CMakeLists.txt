# Shared audio front end (docs/architecture/audio.md). The pure C core (yk_dsp, yk_fe, yk_srmodels, yk_ptt) is host
# tested with test/run.sh; yaykit_audio.c is the device side (capture task, ESP-SR nsnet2 + vadnet, preroll, the
# push-to-talk recording), yk_opus.c the microphone's Opus encoder.
idf_component_register(SRCS "yaykit_audio.c" "yk_opus.c" "yk_dsp.c" "yk_fe.c" "yk_srmodels.c" "yk_onset.c" "yk_ptt.c"
    INCLUDE_DIRS "include" "."
    PRIV_REQUIRES yaykit_board espressif__esp-sr espressif__esp_audio_codec espressif__esp_codec_dev
                  esp_partition esp_timer console mbedtls)

# The speech models: esp-sr's movemodel.py packs the ones sdkconfig selects into srmodels.bin, which `idf.py flash`
# writes to the `models` partition (above 16 MB; esp-sr's own rule only knows a partition named `model`).
if(CONFIG_PARTITION_TABLE_CUSTOM AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    partition_table_get_partition_info(models_offset "--partition-name models" "offset")
    if(models_offset)
        idf_component_get_property(sr_dir espressif__esp-sr COMPONENT_DIR)
        idf_build_get_property(build_dir BUILD_DIR)
        idf_build_get_property(python PYTHON)
        set(srmodels ${build_dir}/srmodels/srmodels.bin)
        add_custom_command(OUTPUT ${srmodels}
            COMMAND ${python} ${sr_dir}/model/movemodel.py -d1 ${SDKCONFIG} -d2 ${sr_dir} -d3 ${build_dir}
            DEPENDS ${SDKCONFIG}
            COMMENT "Packing the speech models (srmodels.bin)"
            VERBATIM)
        add_custom_target(yk_srmodels ALL DEPENDS ${srmodels})
        esptool_py_flash_to_partition(flash "models" "${srmodels}")
    endif()
endif()

set(vad_sources "")
if(CONFIG_JULIA_CAPTURE_VAD_ENABLE)
    list(APPEND vad_sources "lc_vad.c")
endif()
idf_component_register(
    SRCS "board_audio.c" "local_capture.c" "lc_spectrum.c" ${vad_sources}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES driver esp_driver_i2s esp_driver_gpio freertos espressif__esp-dsp espressif__esp-sr
)

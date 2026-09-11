# 将服务器根证书嵌入最终固件，供 HTTPS OTA 客户端进行证书链校验。

idf_build_get_property(project_dir PROJECT_DIR)

set(embed_files
    ${project_dir}/server_certs/ca_cert.pem
)

set(embed_binary_files
    "ui/generated/clips/LISTEN.bin"
    "ui/generated/clips/THINK.bin"
    "ui/generated/clips/SPEAK.bin"
    ${project_dir}/network_disconnected_16k_mono_16bit.wav
    ${project_dir}/low_battery_16k_mono_16bit.wav
    ${project_dir}/bye_no_bother_16k_mono_16bit.wav
    ${project_dir}/goodnight_16k_mono_16bit.wav
    ${project_dir}/upgrade_start_16k_mono_16bit.wav
    ${project_dir}/upgrade_success_16k_mono_16bit.wav
    ${project_dir}/upgrade_failed_16k_mono_16bit.wav
    ${project_dir}/restart_after_issue_16k_mono_16bit.wav
)

if(CONFIG_COMM_DEVICE_AUTH_CERTIFICATE)
    list(APPEND embed_files
        ${project_dir}/server_certs/device_client_cert.pem
        ${project_dir}/server_certs/device_client_key.pem
    )
endif()

set(srcs
    # 应用入口与顶层编排。
    "app/main.c"

    # 网络生命周期、MQTT 通信与公共 HTTPS 下载。
    "network/network_lifecycle.c"
    "network/mqtt_comm.c"
    "network/http_downloader.c"
    "network/download_protocol.c"

    # 语音服务：纯传输层 + 业务协议 + URI 映射 + 演示。
    "voice/wss_transport.c"
    "voice/wss_tx_writer.c"
    "voice/voice_uplink_ring.c"
    "voice/voice_uplink_pump.c"
    "voice/voice_service.c"
    "voice/voice_state_sync.c"
    "voice/voice_control_guard.c"
    "voice/voice_playback.c"
    "voice/pcm_buffer.c"
    "voice/voice_uri.c"
    "voice/voice_push_demo.c"

    # 存储与板级硬件。
    "storage/sd_card.c"
    "hardware/tca9554.c"
    "hardware/julia_battery.c"
    "hardware/julia_power.c"
    "hardware/breathing_led.c"
    "hardware/julia_led.c"
    "hardware/pcf85063_shared.c"
    "hardware/rtc_calendar.c"
    "hardware/qmi8658_shared.c"

    # Local wall-clock restore and SNTP-to-RTC synchronization.
    "context/julia_time.c"
    "context/julia_night_schedule.c"
    "context/julia_motion.c"

    # L0/L1 display: ST77916 + LVGL + static layered avatar.
    "display/esp_lcd_st77916.c"
    "display/julia_display.c"
    "lvgl_port/lvgl_port.c"
    "ui/avatar_rle.c"
    "ui/julia_avatar.c"
    "ui/avatar_parts/avatar_eyes.c"
    "ui/avatar_parts/avatar_mouth.c"
    "ui/generated/avatar_layers/avatar_face_base.c"
    "ui/generated/avatar_layers/avatar_face_doze.c"
    "ui/generated/avatar_layers/avatar_chroma_assets.c"
    "ui/julia_backlight.c"
    "app/julia_idle_display.c"

    # 固件 OTA 模块。
    "ota/ota_boot_flow.c"
    "ota/ota_boot_health.c"
    "ota/ota_control_plane.c"
    "ota/ota_engine.c"
    "ota/ota_report.c"
    "ota/ota_state_store.c"
    "ota/ota_stability.c"

    # 音频素材下载模块。
    "audio/audio_control_plane.c"
    "audio/audio_engine.c"
    "audio/audio_service.c"

    "fsm/julia_fsm.c"
    "fsm/julia_fsm_runtime.c"
    "fsm/julia_fault.c"

)
set(priv_requires
    esp_http_client
    http_parser
    app_update
    bootloader_support
    esp_hw_support
    spi_flash
    efuse
    heap
    nvs_flash
    esp_driver_gpio
    esp_netif
    esp_wifi
    protocol_examples_common
    esp_timer
    esp_driver_rmt 
    esp_driver_ledc
    esp_pm
     
    json
    mbedtls
    mqtt
    esp-tls
    lwip
    fatfs
    sdmmc
    esp_driver_sdmmc
    esp_driver_i2c
    esp_driver_spi
    esp_driver_i2s
    esp_adc
    esp_lcd
    esp_mm
    lvgl__lvgl
    julia_board_audio
)

# Server-side wake is the delivery default: the firmware continuously uploads
# IDLE PCM and the server sends MIC_START only after detecting the wake word.
# Keep the local WakeNet source and dependency behind one switch for rollback
# or for a future custom "你好小科" model.
if(NOT CONFIG_JULIA_SERVER_WAKE_ENABLE)
    list(APPEND srcs "voice/wake_detector.c")
    list(APPEND priv_requires espressif__esp-sr)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
        "app"
        "audio"
        "context"
        "display"
        "hardware"
        "lvgl_port"
        "network"
        "ota"
        "storage"
        "memory"
        "fsm"
        "ui"
        "ui/avatar_parts"
        "ui/generated/avatar_layers"
        "voice"
    PRIV_REQUIRES ${priv_requires}
    EMBED_TXTFILES ${embed_files}
    EMBED_FILES ${embed_binary_files}
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    JULIA_BACKLIGHT_GPIO=5
    JULIA_LED_GPIO=4
)

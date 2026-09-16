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
    # 应用入口与启动装配。
    "app/main.c"

    # 行为状态、故障恢复与本地行为策略。
    "behavior/julia_fault.c"
    "behavior/julia_fsm.c"
    "behavior/julia_fsm_runtime.c"
    "behavior/julia_idle_display.c"
    "behavior/julia_motion.c"
    "behavior/julia_night_schedule.c"
    "behavior/julia_quiet_power.c"

    # 网络生命周期、MQTT、HTTPS 与 WebSocket 传输。
    "network/download_protocol.c"
    "network/http_downloader.c"
    "network/mqtt_comm.c"
    "network/network_lifecycle.c"
    "network/wss/wss_transport.c"
    "network/wss/wss_tx_writer.c"

    # 语音业务、采集、播放、协议与上行。
    "voice/capture/voice_local_capture.c"
    "voice/demo/voice_push_demo.c"
    "voice/playback/pcm_buffer.c"
    "voice/playback/voice_playback.c"
    "voice/protocol/voice_control_guard.c"
    "voice/protocol/voice_state_sync.c"
    "voice/protocol/voice_uri.c"
    "voice/uplink/voice_uplink_pump.c"
    "voice/uplink/voice_uplink_ring.c"
    "voice/voice_service.c"

    # 板级外设与系统时间。
    "hardware/breathing_led.c"
    "hardware/julia_battery.c"
    "hardware/julia_led.c"
    "hardware/julia_power.c"
    "hardware/pcf85063_shared.c"
    "hardware/qmi8658_shared.c"
    "hardware/rtc_calendar.c"
    "hardware/tca9554.c"
    "time/julia_time.c"

    # LCD、背光、LVGL 与界面呈现。
    "display/esp_lcd_st77916.c"
    "display/julia_backlight.c"
    "display/julia_display.c"
    "display/lvgl_port/lvgl_port.c"
    "ui/avatar_rle.c"
    "ui/julia_avatar.c"
    "ui/avatar_parts/avatar_eyes.c"
    "ui/avatar_parts/avatar_mouth.c"
    "ui/generated/avatar_layers/avatar_face_base.c"
    "ui/generated/avatar_layers/avatar_face_doze.c"
    "ui/generated/avatar_layers/avatar_chroma_assets.c"

    # 固件升级。
    "ota/ota_boot_flow.c"
    "ota/ota_boot_health.c"
    "ota/ota_control_plane.c"
    "ota/ota_engine.c"
    "ota/ota_report.c"
    "ota/ota_stability.c"
    "ota/ota_state_store.c"

    # 音频素材清单与下载。
    "audio_assets/audio_control_plane.c"
    "audio_assets/audio_engine.c"
    "audio_assets/audio_service.c"

    # "storage/sd_card.c"  # 暂停 SD 功能，保留以便恢复。
)
set(priv_requires
    esp_http_client
    # IDF collects dependencies before Kconfig evaluation; only the diagnostic
    # source/entry point is conditional. Product firmware starts no HTTP server.
    esp_http_server
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
    # sdmmc             # 随 SD 驱动暂停。
    # esp_driver_sdmmc
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
    list(APPEND srcs "voice/capture/wake_detector.c")
    list(APPEND priv_requires espressif__esp-sr)
endif()

if(CONFIG_JULIA_IMU_LOGGER_ENABLE)
    list(APPEND srcs "diagnostics/imu_logger.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
        "app"
        "diagnostics"
        "audio_assets"
        "behavior"
        "time"
        "display"
        "hardware"
        "display/lvgl_port"
        "network"
        "network/wss"
        "ota"
        "storage"
        "ui"
        "ui/avatar_parts"
        "ui/generated/avatar_layers"
        "voice"
        "voice/capture"
        "voice/playback"
        "voice/protocol"
        "voice/uplink"
        "voice/demo"
    PRIV_REQUIRES ${priv_requires}
    EMBED_TXTFILES ${embed_files}
    EMBED_FILES ${embed_binary_files}
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    JULIA_BACKLIGHT_GPIO=5
    JULIA_LED_GPIO=4
)

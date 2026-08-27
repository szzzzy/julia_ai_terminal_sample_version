# 将服务器根证书嵌入最终固件，供 HTTPS OTA 客户端进行证书链校验。

idf_build_get_property(project_dir PROJECT_DIR)

set(embed_files
    ${project_dir}/server_certs/ca_cert.pem
)

if(CONFIG_COMM_DEVICE_AUTH_CERTIFICATE)
    list(APPEND embed_files
        ${project_dir}/server_certs/device_client_cert.pem
        ${project_dir}/server_certs/device_client_key.pem
    )
endif()

set(srcs
    # 应用入口与顶层编排。
    "app/native_ota_example.c"

    # 网络生命周期、MQTT 通信与公共 HTTPS 下载。
    "network/network_lifecycle.c"
    "network/mqtt_comm.c"
    "network/http_downloader.c"

    # 语音服务：纯传输层 + 业务协议 + URI 映射 + 演示。
    "voice/wss_transport.c"
    "voice/voice_service.c"
    "voice/voice_uri.c"
    "voice/voice_push_demo.c"

    # 存储与板级硬件。
    "storage/sd_card.c"
    "hardware/tca9554.c"
    "hardware/breathing_led.c"
    "hardware/julia_led.c"

    # L0/L1 display: ST77916 + LVGL + static layered avatar.
    "display/esp_lcd_st77916.c"
    "display/julia_display.c"
    "lvgl_port/lvgl_port.c"
    "ui/julia_avatar.c"
    "ui/avatar_parts/avatar_eyes.c"
    "ui/avatar_parts/avatar_mouth.c"
    "ui/generated/avatar_layers/avatar_face_base.c"
    "ui/generated/avatar_layers/avatar_chroma_assets.c"

    # 本地唤醒词（WakeNet "你好小智"）：检测到后自动开麦推流。
    "voice/wake_detector.c"

    # 固件 OTA 模块。
    "ota/ota_boot_health.c"
    "ota/ota_control_plane.c"
    "ota/ota_report.c"
    "ota/ota_state_store.c"
    "ota/ota_stability.c"

    # 音频素材下载模块。
    "audio/audio_control_plane.c"
    "audio/audio_engine.c"
    "audio/audio_service.c"
)
set(priv_requires
    esp_http_client
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
    esp_timer
    esp_driver_rmt 
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
    esp_lcd
    esp_mm
    lvgl__lvgl
    julia_board_audio
    espressif__esp-sr
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "."
        "app"
        "audio"
        "display"
        "hardware"
        "lvgl_port"
        "network"
        "ota"
        "storage"
        "ui"
        "ui/avatar_parts"
        "ui/generated/avatar_layers"
        "voice"
    PRIV_REQUIRES ${priv_requires}
    EMBED_TXTFILES ${embed_files}
)

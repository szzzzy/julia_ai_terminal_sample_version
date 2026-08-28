/**
 * @file    main.c
 * @brief   Julia 固件应用入口与顶层服务初始化顺序。
 */
#include "esp_err.h"
#include "esp_log.h"
#include "breathing_led.h"
#include "julia_led.h"

#include "audio_service.h"
#include "board_audio.h"
#include "julia_avatar.h"
#include "julia_night_schedule.h"
#include "julia_motion.h"
#include "julia_time.h"
#include "julia_display.h"
#include "julia_idle_display.h"
#include "mqtt_comm.h"
#include "network_lifecycle.h"
#include "ota_boot_flow.h"
#include "sd_card.h"
#include "voice_service.h"
#include "wake_detector.h"
#include "julia_backlight.h"
#include "julia_fsm.h"
#include "julia_fsm_runtime.h"
#if CONFIG_VOICE_PUSH_DEMO_ENABLE
#include "voice_push_demo.h"
#endif

static const char *TAG = "app_main";

/**
 * @brief 初始化本地服务并启动后台网络生命周期。
 *
 * OTA 的下载、Flash 写入和启动验收细节由 ota 模块封装；此处只保留顶层初始化
 * 顺序和各业务模块的基本调用，使应用入口可以直接反映系统组成。
 */
void app_main(void)
{
    ESP_LOGI(TAG, "Julia application start");

    ota_boot_flow_run();

    esp_err_t err;
    /* Boot presentation is deliberately outside the behaviour FSM. Only the
     * minimum display stack is brought up first; all voice/network/context
     * services start after the one-shot eye sequence has completed. */
    err = julia_backlight_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Julia backlight init failed: %s", esp_err_to_name(err));
    }
    err = julia_display_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Julia display init failed: %s", esp_err_to_name(err));
    } else {
        err = julia_avatar_init();
        if (err == ESP_OK) {
            esp_err_t boot_err = julia_avatar_play_boot_sequence();
            if (boot_err != ESP_OK) {
                ESP_LOGW(TAG, "Julia boot eye sequence incomplete: %s",
                         esp_err_to_name(boot_err));
                julia_backlight_set(100);
            }
            err = julia_idle_display_init();
            if (err != ESP_OK) {
                julia_backlight_set(100);
            }
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Julia avatar init failed: %s", esp_err_to_name(err));
        }
    }

    /* 语音服务装配：注册 MQTT 语音命令 topic（非 critical，不影响 OTA 就绪）。
     * WSS 客户端不在此启动：取得 IPv4 后由下方注册的 ip_ready 回调启动。 */
    err = voice_service_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Voice service init failed: %s", esp_err_to_name(err));
    }
    /* 板级音频（最小包 mic_test.c 抽取）：MIC 走 WSS 上行、SPKS/SPKE 下行控制。 */
    err = board_audio_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Board audio init failed: %s", esp_err_to_name(err));
    } else {
        err = voice_service_init_board_audio();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Voice-board audio wiring failed: %s", esp_err_to_name(err));
        }
    }

    /* The existing 6-main/20-sub-state FSM is the sole behaviour-state owner.
     * States without phase-one artwork safely fall back to the default portrait. */
    err = julia_fsm_runtime_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Julia FSM runtime init failed: %s", esp_err_to_name(err));
    }

    /* 本地唤醒词（WakeNet "你好小智"）：检测到后自动 MIC_START 推流。
     * 依赖 "model" 分区（构建时 esp-sr 自动打包 srmodels.bin）。 */
    err = wake_detector_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wake detector init failed: %s", esp_err_to_name(err));
    }
    /* 音频服务装配点（当前无自有状态，与 OTA 服务层保持一致的初始化顺序）。 */
    err = audio_service_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Audio service init failed: %s", esp_err_to_name(err));
    }

    /* RTC restore is local and best-effort. SNTP starts later from the IP-ready
     * callback, then writes the synchronized local time back to PCF85063. */
    err = julia_time_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Julia time context init failed: %s", esp_err_to_name(err));
    }
#if CONFIG_JULIA_NIGHT_SLEEP_ENABLE
    err = julia_night_schedule_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Night sleep schedule init failed: %s", esp_err_to_name(err));
    }
#endif
#if CONFIG_JULIA_IMU_MOTION_ENABLE
    err = julia_motion_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IMU motion detector init failed: %s", esp_err_to_name(err));
    }
#endif

    /* SD 卡（SPI，FAT 挂载到 /sdcard）：供 voice_service 的 "SD:/<name>" 文件
     * 推送与显式传输演示使用；不依赖网络，挂载失败会自动重试。 */
    err = sd_card_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card monitor not started: %s", esp_err_to_name(err));
    }

    /* 取得 IPv4 后按注册顺序启动 MQTT 与 WSS 语音服务；任一启动失败都由网络
     * 生命周期任务按独立的有界退避重试，服务之间互不干扰。 */
    err = network_lifecycle_register_ip_ready(mqtt_comm_ip_ready, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT IP-ready callback registration failed: %s",
                 esp_err_to_name(err));
    }
    err = network_lifecycle_register_ip_ready(voice_service_ip_ready, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Voice IP-ready callback registration failed: %s",
                 esp_err_to_name(err));
    }
    err = network_lifecycle_register_ip_ready(julia_time_ip_ready, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Time-sync callback registration failed: %s",
                 esp_err_to_name(err));
    }

    /* 网络不可用绝不能阻断本地应用或影响 pending 镜像验收。Wi-Fi 管理器在后台
     * 永久重连；只有取得 IPv4 后才会调用已注册的服务启动回调。 */
    ESP_LOGI(TAG, "Starting background Wi-Fi lifecycle");
    err = network_lifecycle_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Background Wi-Fi lifecycle is unavailable: %s; local application remains active",
                 esp_err_to_name(err));
    }

    /* 设备主动推送演示（默认关闭）：WSS 会话默认由云端驱动（服务端发
     * FILE_SEND，设备推送）；启用演示后设备才会主动推测试音频列表。 */
#if CONFIG_VOICE_PUSH_DEMO_ENABLE
    err = voice_push_demo_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Voice push demo not started: %s", esp_err_to_name(err));
    }
#endif
}

/**
 * @file    main.c
 * @brief   Julia 固件应用入口与顶层服务初始化顺序。
 */
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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
#if !CONFIG_JULIA_SERVER_WAKE_ENABLE
#include "wake_detector.h"
#endif
#include "julia_backlight.h"
#include "julia_fsm.h"
#include "julia_fsm_runtime.h"
#include "julia_fault.h"
#if CONFIG_VOICE_PUSH_DEMO_ENABLE
#include "voice_push_demo.h"
#endif

static const char *TAG = "app_main";
static SemaphoreHandle_t s_boot_animation_done;
static portMUX_TYPE s_boot_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_runtime_ready;
static bool s_voice_ready;

static void boot_animation_task(void *arg)
{
    (void)arg;
    int64_t started = esp_timer_get_time();
    ESP_LOGI(TAG, "Boot animation start");
    esp_err_t err = julia_avatar_play_boot_sequence();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Boot animation incomplete: %s", esp_err_to_name(err));
        julia_backlight_set(100);
    }
    ESP_LOGI(TAG, "Boot animation done in %lldms",
             (long long)((esp_timer_get_time() - started) / 1000));
    xSemaphoreGive(s_boot_animation_done);
    vTaskDelete(NULL);
}

static esp_err_t boot_mqtt_ip_ready(void *arg)
{
    portENTER_CRITICAL(&s_boot_lock);
    bool ready = s_runtime_ready;
    portEXIT_CRITICAL(&s_boot_lock);
    return ready ? mqtt_comm_ip_ready(arg) : ESP_ERR_INVALID_STATE;
}

static esp_err_t boot_voice_ip_ready(void *arg)
{
    portENTER_CRITICAL(&s_boot_lock);
    bool ready = s_runtime_ready && s_voice_ready;
    portEXIT_CRITICAL(&s_boot_lock);
    return ready ? voice_service_ip_ready(arg) : ESP_ERR_INVALID_STATE;
}

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
    esp_err_t visual_error = ESP_OK;
    esp_err_t voice_error = ESP_OK;
    esp_err_t audio_error = ESP_OK;
    esp_err_t idle_display_error = ESP_OK;
    bool backlight_ready = false;
    bool visual_ready = false;
    int64_t boot_started = esp_timer_get_time();
    /* 显示与共享 I2C 基础设施只初始化一次；动画随后在独立任务中执行，
     * 主任务同时初始化不直接争用画面的服务。 */
    err = julia_backlight_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Julia backlight init failed: %s", esp_err_to_name(err));
        visual_error = err;
    } else {
        backlight_ready = true;
    }
    err = julia_display_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Julia display init failed: %s", esp_err_to_name(err));
        if (visual_error == ESP_OK) visual_error = err;
    } else {
        err = julia_avatar_init();
        if (err == ESP_OK) {
            s_boot_animation_done = xSemaphoreCreateBinary();
            if (s_boot_animation_done == NULL ||
                xTaskCreate(boot_animation_task, "boot_animation", 4096, NULL, 3, NULL) != pdPASS) {
                if (s_boot_animation_done != NULL) {
                    vSemaphoreDelete(s_boot_animation_done);
                    s_boot_animation_done = NULL;
                }
                ESP_LOGW(TAG, "No animation task resources; using sequential startup");
                if (julia_avatar_play_boot_sequence() != ESP_OK) julia_backlight_set(100);
            }
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Julia avatar init failed: %s", esp_err_to_name(err));
            if (visual_error == ESP_OK) visual_error = err;
        } else if (backlight_ready) {
            visual_ready = true;
        }
    }

    /* 语音服务装配：注册 MQTT 语音命令 topic（非 critical，不影响 OTA 就绪）。
     * WSS 客户端不在此启动：取得 IPv4 后由下方注册的 ip_ready 回调启动。 */
    err = voice_service_init();
    bool voice_initialized = err == ESP_OK;
    voice_error = err;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Voice service init failed: %s", esp_err_to_name(err));
    }
    /* 板级音频（最小包 mic_test.c 抽取）：MIC 走 WSS 上行、SPKS/SPKE 下行控制。 */
    err = board_audio_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Board audio init failed: %s", esp_err_to_name(err));
        audio_error = err;
    } else {
        err = voice_service_init_board_audio();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Voice-board audio wiring failed: %s", esp_err_to_name(err));
            audio_error = err;
        }
    }
    bool audio_ready = err == ESP_OK;

    /* 音频服务装配点（当前无自有状态，与 OTA 服务层保持一致的初始化顺序）。 */
    err = audio_service_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Audio service init failed: %s", esp_err_to_name(err));
    }

    /* RTC 恢复属于本地尽力而为操作；取得 IP 后再启动 SNTP，并把同步时间写回 PCF85063。 */
    err = julia_time_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Julia time context init failed: %s", esp_err_to_name(err));
    }
    /* SDMMC 挂载复用已经初始化的 I2C 扩展器，不操作启动动画；失败只影响文件服务。 */
    err = sd_card_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card monitor not started: %s", esp_err_to_name(err));
    }

    /* 取得 IPv4 后按注册顺序启动 MQTT 与 WSS 语音服务；任一启动失败都由网络
     * 生命周期任务按独立的有界退避重试，服务之间互不干扰。 */
    err = network_lifecycle_register_ip_ready(boot_mqtt_ip_ready, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MQTT IP-ready callback registration failed: %s",
                 esp_err_to_name(err));
    }
    err = network_lifecycle_register_ip_ready(boot_voice_ip_ready, NULL);
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

    ESP_LOGI(TAG, "Parallel services initialized in %lldms; joining boot presentation",
             (long long)((esp_timer_get_time() - boot_started) / 1000));
    if (s_boot_animation_done != NULL) {
        /* 动画刷新和渐变等待均有界；该汇合点不等待网络，避免 FSM/UI 恢复覆盖启动帧。 */
        xSemaphoreTake(s_boot_animation_done, portMAX_DELAY);
        vSemaphoreDelete(s_boot_animation_done);
        s_boot_animation_done = NULL;
    }
    err = julia_idle_display_init();
    idle_display_error = err;
    bool idle_display_ready = err == ESP_OK;
    if (!idle_display_ready)
        ESP_LOGW(TAG, "Idle display init failed: %s", esp_err_to_name(err));
    bool boot_dependencies_ready = visual_ready && audio_ready &&
                                   voice_initialized && idle_display_ready;
    err = julia_fsm_runtime_init(boot_dependencies_ready);
    esp_err_t fsm_error = err;
    bool fsm_ready = err == ESP_OK;
    if (!fsm_ready) ESP_LOGW(TAG, "FSM runtime init failed: %s", esp_err_to_name(err));

    if (fsm_ready && boot_dependencies_ready) {
#if CONFIG_JULIA_NIGHT_SLEEP_ENABLE
        err = julia_night_schedule_init();
        if (err != ESP_OK) ESP_LOGW(TAG, "Night schedule init failed: %s", esp_err_to_name(err));
#endif
#if CONFIG_JULIA_IMU_MOTION_ENABLE
        err = julia_motion_init();
        if (err != ESP_OK) ESP_LOGW(TAG, "Motion init failed: %s", esp_err_to_name(err));
#endif
#if !CONFIG_JULIA_SERVER_WAKE_ENABLE
        if (audio_ready && voice_initialized) {
            err = wake_detector_init();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Wake detector init failed: %s", esp_err_to_name(err));
                (void)julia_fsm_runtime_raise_fault(JULIA_FAULT_VOICE_INIT, err);
            }
        }
#endif
    }
    portENTER_CRITICAL(&s_boot_lock);
    s_voice_ready = boot_dependencies_ready && fsm_ready;
    s_runtime_ready = fsm_ready;
    portEXIT_CRITICAL(&s_boot_lock);
    if (fsm_ready && !boot_dependencies_ready) {
        julia_fault_reason_t reason = JULIA_FAULT_CRITICAL_INIT;
        esp_err_t fault_error = ESP_FAIL;
        if (!visual_ready) {
            reason = JULIA_FAULT_DISPLAY_INIT;
            fault_error = visual_error;
        } else if (!audio_ready) {
            reason = JULIA_FAULT_AUDIO_INIT;
            fault_error = audio_error;
        } else if (!voice_initialized) {
            reason = JULIA_FAULT_VOICE_INIT;
            fault_error = voice_error;
        } else if (!idle_display_ready) {
            reason = JULIA_FAULT_DISPLAY_INIT;
            fault_error = idle_display_error;
        }
        err = julia_fsm_runtime_raise_fault(reason, fault_error);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot enqueue S7 fault: %s", esp_err_to_name(err));
        }
    } else if (!fsm_ready) {
        /* FSM 无法创建时没有运行队列可进入 S7；仍记录同一格式快照后直接复位。 */
        (void)julia_fault_record(JULIA_FAULT_FSM_RUNTIME_INIT, fsm_error,
                                 JULIA_MAIN_STATE_S0_BOOT,
                                 JULIA_S2_SUB_STATE_NONE);
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_IDLE);
        julia_avatar_set_dozing(true);
        julia_backlight_set(40);
        if (!julia_fault_reset_allowed()) {
            ESP_LOGE(TAG, "FSM 初始化连续失败，保持 S7 等待售后处理");
            while (1) vTaskDelay(pdMS_TO_TICKS(1000));
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }
    network_lifecycle_retry_services();
    ESP_LOGI(TAG, "Boot interaction gate open in %lldms (voice_ready=%u)",
             (long long)((esp_timer_get_time() - boot_started) / 1000), (unsigned)s_voice_ready);

    /* 设备主动推送演示（默认关闭）：WSS 会话默认由云端驱动（服务端发
     * FILE_SEND，设备推送）；启用演示后设备才会主动推测试音频列表。 */
#if CONFIG_VOICE_PUSH_DEMO_ENABLE
    err = voice_push_demo_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Voice push demo not started: %s", esp_err_to_name(err));
    }
#endif
}

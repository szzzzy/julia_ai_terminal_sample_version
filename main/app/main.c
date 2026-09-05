/**
 * @file    main.c
 * @brief   按安全顺序启动屏幕、声音、网络和设备行为，并在关键能力失败时进入故障状态。
 *
 * 开机阶段按显示、音频、存储和 Wi-Fi 的顺序错峰启用高电流负载。动画结束且显示、
 * 麦克风、扬声器、语音服务和空闲策略都可用后，设备才开放 MQTT/WSS 交互并从
 * 开机进入默认待机。Wi-Fi 或服务器暂时不可用不会阻塞本地界面，后台会继续恢复。
 */
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "breathing_led.h"
#include "julia_led.h"

#include "audio_service.h"
#include "board_audio.h"
#include "julia_avatar.h"
#include "julia_night_schedule.h"
#include "julia_motion.h"
#include "julia_battery.h"
#include "julia_power.h"
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
static portMUX_TYPE s_boot_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_runtime_ready;
static bool s_voice_ready;

static void battery_status_updated(const julia_battery_status_t *status, void *ctx)
{
    (void)ctx;
    if (status == NULL || !status->valid) return;
    julia_avatar_set_battery_status(
        status->present,
        status->state == JULIA_BATTERY_STATE_CHARGING,
        status->state == JULIA_BATTERY_STATE_LOW,
        status->percent,
        status->voltage_mv);
}

static void boot_stage_settle(const char *stage)
{
    julia_battery_log_stage(stage);
    if (CONFIG_JULIA_BOOT_STAGE_DELAY_MS > 0) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_JULIA_BOOT_STAGE_DELAY_MS));
    }
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
 * @brief 完成整机启动，并确保服务器消息不会早于本地显示和声音能力生效。
 *
 * 首先确认待验收的新固件是否可以继续运行，再按显示、声音、存储、Wi-Fi 的顺序
 * 错峰启动；每个阶段记录 BAT_ADC 电压。所有关键能力就绪后才开放交互。
 * 关键本机能力失败会留下故障记录并受控复位；网络失败仅由后台重连处理。
 */
void app_main(void)
{
    esp_err_t power_hold_err = julia_power_hold_enable();
    if (power_hold_err != ESP_OK) {
        ESP_LOGE(TAG, "Battery power hold init failed: %s",
                 esp_err_to_name(power_hold_err));
    }
    esp_err_t power_management_err = julia_power_management_init();
    if (power_management_err != ESP_OK) {
        ESP_LOGW(TAG, "Dynamic frequency scaling init failed: %s",
                 esp_err_to_name(power_management_err));
    }
    esp_err_t battery_diagnostic_err = julia_battery_diagnostics_init();
    if (battery_diagnostic_err != ESP_OK &&
        battery_diagnostic_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Battery diagnostics unavailable: %s",
                 esp_err_to_name(battery_diagnostic_err));
    }
    julia_battery_log_stage("power_hold");
    ESP_LOGI(TAG, "Julia application start");

    ota_boot_flow_run();
    boot_stage_settle("ota_ready");

    esp_err_t err;
    esp_err_t visual_error = ESP_OK;
    esp_err_t voice_error = ESP_OK;
    esp_err_t audio_error = ESP_OK;
    esp_err_t idle_display_error = ESP_OK;
    bool backlight_ready = false;
    bool visual_ready = false;
    int64_t boot_started = esp_timer_get_time();
    /* 显示和动画完成后才启用音频；低亮度动画不再与其它外设上电并行。 */
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
            ESP_LOGI(TAG, "Boot animation start at limited brightness");
            esp_err_t animation_err = julia_avatar_play_boot_sequence();
            if (animation_err != ESP_OK) {
                ESP_LOGW(TAG, "Boot animation incomplete: %s",
                         esp_err_to_name(animation_err));
                julia_backlight_set(CONFIG_JULIA_BOOT_BRIGHTNESS_PERCENT);
            }
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Julia avatar init failed: %s", esp_err_to_name(err));
            if (visual_error == ESP_OK) visual_error = err;
        } else if (backlight_ready) {
            visual_ready = true;
        }
    }
    boot_stage_settle("display_ready");

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
    boot_stage_settle("audio_ready");

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
    boot_stage_settle("storage_ready");

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

    ESP_LOGI(TAG, "Staged local services initialized in %lldms",
             (long long)((esp_timer_get_time() - boot_started) / 1000));
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
    esp_err_t wake_error = ESP_OK;
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
            wake_error = err;
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Wake detector init failed: %s", esp_err_to_name(err));
            }
        }
#endif
    }
    ota_boot_flow_complete(boot_dependencies_ready && fsm_ready && wake_error == ESP_OK);
    if (fsm_ready && wake_error != ESP_OK) {
        (void)julia_fsm_runtime_raise_fault(JULIA_FAULT_VOICE_INIT, wake_error);
    }
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
            ESP_LOGE(TAG, "Cannot enqueue S7.2 fault: %s", esp_err_to_name(err));
        }
    } else if (!fsm_ready) {
        /* FSM 无法创建时没有运行队列可进入 S7.2；仍记录同一格式快照后直接复位。 */
        (void)julia_fault_record(JULIA_FAULT_FSM_RUNTIME_INIT, fsm_error,
                                 JULIA_MAIN_STATE_S0_BOOT,
                                 JULIA_S2_SUB_STATE_NONE);
        julia_avatar_set_dialog_phase(JULIA_AVATAR_DIALOG_IDLE);
        julia_avatar_set_dozing(true);
        julia_backlight_set(CONFIG_JULIA_BOOT_BRIGHTNESS_PERCENT);
        if (!julia_fault_reset_allowed()) {
            ESP_LOGE(TAG, "FSM 初始化连续失败，保持 S7.2 等待售后处理");
            while (1) vTaskDelay(pdMS_TO_TICKS(1000));
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_JULIA_FAULT_RESET_DELAY_MS));
        esp_restart();
    }

    /* Wi-Fi 最后启动，避免 RF 校准/扫描与显示、音频上电落在同一电流尖峰。网络
     * 不可用仍由后台永久重试，不阻断本地应用或 pending 镜像验收。 */
    boot_stage_settle("runtime_ready");
    ESP_LOGI(TAG, "Starting background Wi-Fi lifecycle after local startup");
    esp_err_t network_err = network_lifecycle_start();
    err = network_err;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Background Wi-Fi lifecycle is unavailable: %s; local application remains active",
                 esp_err_to_name(err));
    }
    julia_battery_log_stage("wifi_started");
    if (CONFIG_JULIA_BOOT_SETTLE_DELAY_MS > 0) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_JULIA_BOOT_SETTLE_DELAY_MS));
    }
    julia_battery_log_stage("wifi_settled");

    esp_err_t runtime_power_err = julia_power_runtime_profile_enable();
    if (runtime_power_err != ESP_OK) {
        ESP_LOGW(TAG, "Runtime CPU profile failed: %s",
                 esp_err_to_name(runtime_power_err));
    }
    esp_err_t battery_monitor_err = julia_battery_monitor_start(
        battery_status_updated, NULL);
    if (battery_monitor_err != ESP_OK &&
        battery_monitor_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Battery monitor not started: %s",
                 esp_err_to_name(battery_monitor_err));
    }
    /* RF 关联稳定后才开放 MQTT/WSS，避免 TLS 与首轮 Wi-Fi 扫描/握手重叠。 */
    portENTER_CRITICAL(&s_boot_lock);
    s_voice_ready = boot_dependencies_ready && fsm_ready && wake_error == ESP_OK;
    s_runtime_ready = fsm_ready;
    portEXIT_CRITICAL(&s_boot_lock);
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
    /* 生命周期自身也可能暂时创建失败。保留 app task 重试，无需再申请监督任务栈。 */
    uint32_t retry_ms = CONFIG_NETWORK_WIFI_RETRY_BASE_MS;
    while (network_err != ESP_OK && network_err != ESP_ERR_NOT_SUPPORTED) {
        vTaskDelay(pdMS_TO_TICKS(retry_ms));
        network_err = network_lifecycle_start();
        if (retry_ms < CONFIG_NETWORK_WIFI_RETRY_MAX_MS) {
            retry_ms = retry_ms > CONFIG_NETWORK_WIFI_RETRY_MAX_MS / 2U
                           ? CONFIG_NETWORK_WIFI_RETRY_MAX_MS : retry_ms * 2U;
        }
    }
}

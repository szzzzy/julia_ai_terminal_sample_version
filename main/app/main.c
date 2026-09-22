/**
 * @file main.c
 * @brief 启动协调：基础准备后并行推进本地分支和 Wi-Fi，必要资源成功即握手，动画和判决完成才交出画面。
 *
 * 模块职责：按开机时序发起各模块准备，汇合三个本地分支与网络分支的结果，并请求 julia_fsm_runtime
 * 启动行为状态机、开放云窗口和完成画面交接。
 * 模块边界：不实现子模块的初始化细节，也不直接管理 Wi-Fi/MQTT/WSS 的连接状态；本文件只决定发起
 * 顺序与失败后的降级路径。s_runtime_failed 与 s_cloud_allowed 只经由 s_boot_lock 访问。
 * 关键依赖：boot_coordinator（分支并行与超时）、julia_fsm_runtime（状态与交接判决）、
 * network_lifecycle、ota_boot_flow 以及 audio/display/avatar/电池分支的准备结果。
 * 核心不变量：画面交接必须晚于动画结束，早到的 ONLINE 不得提前覆盖画面或放行业务；
 * 各分支结果只由所属分支写入，coordinator 收到该分支结果后才能读取。
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
#include "julia_quiet_power.h"
#include "julia_time.h"
#include "julia_display.h"
#include "julia_idle_display.h"
#include "mqtt_comm.h"
#include "network_lifecycle.h"
#include "ota_boot_flow.h"
// #include "sd_card.h"  // SD 初始化暂停，恢复时取消注释。
#include "voice_service.h"
#if !CONFIG_JULIA_SERVER_WAKE_ENABLE
#include "wake_detector.h"
#endif
#include "julia_backlight.h"
#include "julia_fsm.h"
#include "julia_fsm_runtime.h"
#include "julia_fault.h"
#if CONFIG_JULIA_IMU_LOGGER_ENABLE
#include "imu_logger.h"
#endif
#if CONFIG_VOICE_PUSH_DEMO_ENABLE
#include "voice_push_demo.h"
#endif

#include "boot_coordinator.h"
#include "tca9554.h"
#include "qmi8658_shared.h"

static const char *TAG = "BOOT";
static portMUX_TYPE s_boot_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_cloud_allowed;
static bool s_runtime_failed;
static esp_err_t s_motion_prepared = ESP_ERR_NOT_SUPPORTED;
/* 各字段由所属分支写入，coordinator 收到该分支结果后才能读取。 */
static julia_fault_reason_t s_audio_fault = JULIA_FAULT_AUDIO_INIT;
static julia_fault_reason_t s_resources_fault = JULIA_FAULT_FSM_RUNTIME_INIT;

static esp_err_t module_result(const char *name, esp_err_t result)
{
    ESP_LOGI(TAG, "module=%s result=%s t=%lldms", name, esp_err_to_name(result),
             (long long)(esp_timer_get_time()/1000));
    return result;
}
static bool cloud_allowed(void)
{
    portENTER_CRITICAL(&s_boot_lock);
    bool allowed = s_cloud_allowed;
    portEXIT_CRITICAL(&s_boot_lock);
    return allowed;
}
static esp_err_t boot_mqtt_ip_ready(void *arg)
{
    return cloud_allowed() ? mqtt_comm_ip_ready(arg) : ESP_ERR_INVALID_STATE;
}
static esp_err_t boot_voice_ip_ready(void *arg)
{
    return cloud_allowed() ? voice_service_ip_ready(arg) : ESP_ERR_INVALID_STATE;
}
static void battery_status_updated(const julia_battery_status_t *status, void *ctx)
{
    (void)ctx;
    if (status == NULL || !status->valid) return;
    julia_avatar_set_battery_status(status->present,
        status->state == JULIA_BATTERY_STATE_LOW, status->percent);
}
static esp_err_t display_branch(void)
{
    esp_err_t err = module_result("backlight", julia_backlight_init());
    if (err != ESP_OK) return err;
    err = module_result("display", julia_display_init());
    if (err != ESP_OK) return err;
    err = module_result("avatar", julia_avatar_init());
    if (err != ESP_OK) return err;
    return ESP_OK;
}
static esp_err_t animation_branch(void)
{
    esp_err_t err = module_result("animation_finished", julia_avatar_play_boot_sequence());
    /* 动画失败可静态交接，显示资源失败不会到达本尾段。 */
    if (err != ESP_OK) julia_backlight_set(CONFIG_JULIA_BOOT_BRIGHTNESS_PERCENT);
    return ESP_OK;
}
static esp_err_t audio_branch(void)
{
    esp_err_t err = module_result("board_audio", board_audio_init());
    if (err != ESP_OK) return err;
    err = module_result("voice_audio", voice_service_init_board_audio());
    if (err != ESP_OK) return err;
    err = module_result("audio_service", audio_service_init());
#if !CONFIG_JULIA_SERVER_WAKE_ENABLE
    if (err == ESP_OK) {
        s_audio_fault = JULIA_FAULT_VOICE_INIT;
        err = module_result("wake_detector", wake_detector_init());
    }
#endif
    return err;
}
static esp_err_t resources_branch(void)
{
    (void)module_result("rtc_attempt_finished", julia_time_init());
    esp_err_t idle = module_result("idle", julia_idle_display_init());
    esp_err_t fsm = module_result("fsm_prepare", julia_fsm_runtime_prepare());
#if CONFIG_JULIA_IMU_MOTION_ENABLE
    s_motion_prepared = module_result("imu_prepare", board_imu_init());
#endif
    s_resources_fault = idle != ESP_OK ? JULIA_FAULT_DISPLAY_INIT : JULIA_FAULT_FSM_RUNTIME_INIT;
    return idle != ESP_OK ? idle : fsm;
}
static void behavior_start(void *arg)
{
    (void)arg;
#if CONFIG_JULIA_NIGHT_SLEEP_ENABLE
    (void)module_result("night_enable", julia_night_schedule_init());
#endif
#if CONFIG_JULIA_IMU_MOTION_ENABLE
    if (s_motion_prepared == ESP_OK)
        (void)module_result("motion_enable", julia_motion_init());
#endif
    esp_err_t err = module_result("quiet_power", julia_quiet_power_init());
    if (err != ESP_OK) {
        /* 保留静默管理创建失败的致命语义；云分支不得随后重新打开门控。 */
        portENTER_CRITICAL(&s_boot_lock);
        s_runtime_failed = true;
        s_cloud_allowed = false;
        portEXIT_CRITICAL(&s_boot_lock);
        (void)julia_fsm_runtime_raise_fault(JULIA_FAULT_CRITICAL_INIT, err);
    }
    vTaskDelete(NULL);
}
static void battery_start(void *arg)
{
    (void)arg;
    (void)module_result("battery_monitor", julia_battery_monitor_start(battery_status_updated, NULL));
    vTaskDelete(NULL);
}
static void ota_accept(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_boot_lock);
    bool healthy = !s_runtime_failed;
    portEXIT_CRITICAL(&s_boot_lock);
    ota_boot_flow_complete(healthy);
    vTaskDelete(NULL);
}
static void boot_failure(julia_fault_reason_t reason, esp_err_t error, bool branches_finished)
{
    portENTER_CRITICAL(&s_boot_lock);
    s_cloud_allowed = false;
    s_runtime_failed = true;
    portEXIT_CRITICAL(&s_boot_lock);
    /* 超时后 worker 可能仍持有驱动锁，不抢占画面、不销毁任务、不开放云服务。 */
    ota_boot_flow_complete(false);
    if (branches_finished && julia_fsm_runtime_raise_fault(reason, error) == ESP_OK)
        return;
    (void)julia_fault_record(reason, error,
                            JULIA_MAIN_STATE_S0_BOOT, JULIA_S2_SUB_STATE_NONE);
    if (julia_fault_reset_allowed()) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_JULIA_FAULT_RESET_DELAY_MS));
        esp_restart();
    }
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
/* 在协调者上下文执行；显示资源、音频和 FSM 已成功，动画可能仍持有画面。 */
static esp_err_t start_cloud_early(void)
{
    ESP_LOGI(TAG, "handshake_dependencies_ready t=%lldms", (long long)(esp_timer_get_time()/1000));
    esp_err_t err = julia_fsm_runtime_start();
    if (err != ESP_OK) return err;
    (void)module_result("runtime_power", julia_power_runtime_profile_enable());
    err = julia_fsm_runtime_start_cloud_window();
    if (err != ESP_OK) return err;
    portENTER_CRITICAL(&s_boot_lock);
    s_cloud_allowed = !s_runtime_failed;
    bool allowed = s_cloud_allowed;
    portEXIT_CRITICAL(&s_boot_lock);
    if (!allowed) return ESP_FAIL;
    ESP_LOGI(TAG, "cloud_gate_open t=%lldms (animation may still be running)",
             (long long)(esp_timer_get_time()/1000));
    network_lifecycle_retry_services();
    return ESP_OK;
}
void app_main(void)
{
    const int64_t entered = esp_timer_get_time();
    esp_err_t hold_error = julia_power_hold_enable();
    ESP_LOGI(TAG, "app_main t=%lldms", (long long)(entered/1000));
    (void)module_result("power_hold", hold_error);
    if (hold_error == ESP_OK)
        (void)module_result("power_key", julia_power_key_start());
    (void)module_result("power_management", julia_power_management_init());
    (void)module_result("battery_diagnostics", julia_battery_diagnostics_init());
    /* 唯一启动采样点；之后 ADC 由电池监测任务独占。 */
    julia_battery_log_stage("power_hold");
    ota_boot_flow_run();
#if CONFIG_JULIA_IMU_LOGGER_ENABLE
    esp_err_t logger = imu_logger_start();
    ota_boot_flow_complete(logger == ESP_OK);
    ESP_ERROR_CHECK(logger);
    return;
#endif
    esp_err_t err = module_result("shared_i2c", tca9554_init());
    /* 首次创建失败时不启动任何总线消费者，避免各分支同时重试创建总线。 */
    if (err != ESP_OK) { boot_failure(JULIA_FAULT_CRITICAL_INIT, err, false); return; }
    err = module_result("voice_assembly", voice_service_init());
    if (err != ESP_OK) { boot_failure(JULIA_FAULT_VOICE_INIT, err, false); return; }
    err = network_lifecycle_register_ip_ready(boot_mqtt_ip_ready, NULL);
    if (err == ESP_OK) err = network_lifecycle_register_ip_ready(boot_voice_ip_ready, NULL);
    if (err == ESP_OK) err = network_lifecycle_register_ip_ready(julia_time_ip_ready, NULL);
    if (err != ESP_OK) { boot_failure(JULIA_FAULT_CRITICAL_INIT, err, false); return; }
    ESP_LOGI(TAG, "base_ready t=%lldms", (long long)(esp_timer_get_time()/1000));
    esp_err_t network_err = module_result("wifi_start", network_lifecycle_start());
    const boot_branch_t branches[3] = {
        {"boot_display", display_branch, 6144, animation_branch},
        {"boot_audio", audio_branch, 8192, NULL},
        {"boot_resources", resources_branch, 4096, NULL},
    };
    boot_result_t results[3];
    err = boot_coordinator_run(branches, results, CONFIG_JULIA_LOCAL_INIT_TIMEOUT_MS, start_cloud_early);
    if (err != ESP_OK) { boot_failure(JULIA_FAULT_CRITICAL_INIT, err, false); return; }
    for (unsigned i=0; i<3; ++i) {
        if (!results[i].completed || results[i].error != ESP_OK) {
            julia_fault_reason_t reason = i == 0 ? JULIA_FAULT_DISPLAY_INIT :
                i == 1 ? s_audio_fault : s_resources_fault;
            boot_failure(reason, results[i].error, true);
            return;
        }
    }

    ESP_LOGI(TAG, "local_ready_s0 t=%lldms elapsed=%lldms", (long long)(esp_timer_get_time()/1000),
             (long long)((esp_timer_get_time()-entered)/1000));
    /* 三项策略在 S0 创建；已有状态准入使夜间不计宽限、IMU 不采样、网络不暂停。
     * S3 迁移的观察者通知唤醒这些任务，无需再次初始化。 */
    if (xTaskCreate(behavior_start, "boot_behavior", 4096, NULL, 3, NULL) != pdPASS) {
        boot_failure(JULIA_FAULT_CRITICAL_INIT, ESP_ERR_NO_MEM, true);
        return;
    }
    /* 动画真正结束后才释放呈现；早到 ONLINE 不能提前覆盖画面或放行业务。 */
    err = julia_fsm_runtime_finish_boot_animation();
    if (err != ESP_OK) { boot_failure(JULIA_FAULT_CRITICAL_INIT, err, true); return; }
    if (xTaskCreate(battery_start, "boot_battery", 3072, NULL, 2, NULL) != pdPASS)
        (void)module_result("battery_task", ESP_ERR_NO_MEM);
    /* 验收失败仍可回滚；待验收期间 OTA 引擎拒绝新升级。普通启动无需创建验收任务。 */
    if (ota_boot_flow_pending()) {
        if (xTaskCreate(ota_accept, "boot_accept", 4096, NULL, 2, NULL) != pdPASS)
            ota_boot_flow_complete(false);
    }
    uint32_t boot_retry_ms = CONFIG_NETWORK_WIFI_RETRY_BASE_MS;
    for (;;) {
        bool retry_network = network_err != ESP_OK && network_err != ESP_ERR_NOT_SUPPORTED;
        err = julia_fsm_runtime_wait_boot_decision(retry_network ? boot_retry_ms : UINT32_MAX);
        if (err == ESP_OK) break;
        if (err != ESP_ERR_TIMEOUT) { boot_failure(JULIA_FAULT_CRITICAL_INIT, err, true); return; }
        /* 等待使用通知，不忙轮询；生命周期创建失败时沿用 app 的退避恢复。 */
        network_err = network_lifecycle_start();
        if (boot_retry_ms < CONFIG_NETWORK_WIFI_RETRY_MAX_MS)
            boot_retry_ms = boot_retry_ms > CONFIG_NETWORK_WIFI_RETRY_MAX_MS / 2U
                ? CONFIG_NETWORK_WIFI_RETRY_MAX_MS : boot_retry_ms * 2U;
    }
    ESP_LOGI(TAG, "local_handoff t=%lldms elapsed=%lldms", (long long)(esp_timer_get_time()/1000),
             (long long)((esp_timer_get_time()-entered)/1000));
#if CONFIG_VOICE_PUSH_DEMO_ENABLE
    (void)module_result("voice_demo", voice_push_demo_start());
#endif
    uint32_t retry_ms = CONFIG_NETWORK_WIFI_RETRY_BASE_MS;
    while (network_err != ESP_OK && network_err != ESP_ERR_NOT_SUPPORTED) {
        vTaskDelay(pdMS_TO_TICKS(retry_ms));
        network_err = network_lifecycle_start();
        if (retry_ms < CONFIG_NETWORK_WIFI_RETRY_MAX_MS)
            retry_ms = retry_ms > CONFIG_NETWORK_WIFI_RETRY_MAX_MS / 2U
                ? CONFIG_NETWORK_WIFI_RETRY_MAX_MS : retry_ms * 2U;
    }
}

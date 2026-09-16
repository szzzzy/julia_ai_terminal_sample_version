/**
 * @file julia_quiet_power.c
 * @brief 把已提交的行为状态翻译成采音（MIC）与网络暂停动作。
 *
 * 负责什么：订阅 julia_fsm_runtime 的状态变化，按状态开关板级 MIC 采音（未启用服务器
 * 唤醒时同步暂停本地唤醒检测），并向 network_lifecycle 发出或撤销暂停请求。
 * 不负责什么：不判断是否应该安静、是否允许唤醒，也不改写任何 FSM 状态；静默、睡眠和
 * OTA 的准入规则只在 FSM 内定义，本模块只执行结果。
 *
 * 当前 capture-v1（CONFIG_JULIA_LOCAL_CAPTURE_ENABLE=y）的不变量：
 *   - S5/S6 保持采音与联网，只有 S0/S8 关闭 MIC；
 *   - 网络从不暂停，set_paused 始终收到 false。
 * 关闭 capture-v1 的旧路径行为不同：S5/S6 停止采音并暂停网络，只能依靠 IMU 搬动或
 * IMU 不可用兜底回到 S3。
 */
#include "julia_quiet_power.h"
#include "board_audio.h"
#include "julia_motion.h"
#include "julia_fsm_runtime.h"
#include "network_lifecycle.h"
#include "voice_playback.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if !CONFIG_JULIA_SERVER_WAKE_ENABLE
#include "wake_detector.h"
#endif

static TaskHandle_t s_task;
#if !CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
static const char *TAG = "QUIET_POWER";
#endif

/* MIC 采音与本地唤醒检测必须同开同关（后者只在未启用服务器唤醒时参与编译）：
 * 只切一路会留下“检测在跑但没有输入”或“有输入但无人接收”的不一致状态。 */
static void set_capture(bool enabled)
{
#if !CONFIG_JULIA_SERVER_WAKE_ENABLE
    wake_detector_set_paused(!enabled);
#endif
    board_audio_mic_set_enabled(enabled);
}

static void quiet_power_task(void *arg)
{
    (void)arg;
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
    /* capture-v1 不变量：网络从不暂停，只有 S0/S8 关闭 MIC，S5/S6 照常采音与联网。
     * 状态只在被通知后才重新读取，因此 julia_quiet_power_notify 必须覆盖所有会改变
     * 采音行为的迁移。 */
    for (;;) {
        julia_main_state_t state = julia_fsm_runtime_get_state();
        network_lifecycle_set_paused(false);
        set_capture(state != JULIA_MAIN_STATE_S8_OTA && state != JULIA_MAIN_STATE_S0_BOOT);
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
#else
    /* 旧路径：安静状态已关闭本地唤醒输入，只能周期性采样状态，并用 IMU 可用性兜底退出。 */
    bool was_quiet = false;
    bool recovering = false;
    int64_t imu_missing_since = 0;
    for (;;) {
        julia_main_state_t state = julia_fsm_runtime_get_state();
        bool quiet = julia_fsm_is_quiet(state);
        if (quiet) {
            if (!was_quiet) {
                recovering = true;
                imu_missing_since = esp_timer_get_time();
                ESP_LOGI(TAG, "quiet entry: stopping capture, waiting for IMU");
            }
            set_capture(false);
            /* 无可用唤醒输入时恢复 S3；运行中持续读失败也不能将设备困在离线状态。 */
            if (!julia_motion_ready()) {
                if (imu_missing_since == 0) imu_missing_since = esp_timer_get_time();
                /* 2000000 us = 2 s：IMU 连续读失败达到该时长才判定无法再用搬动唤醒设备。
                 * 该阈值的来源未确认，只保证以 us 为单位且有界。 */
                if (esp_timer_get_time() - imu_missing_since >= 2000000LL) {
                    ESP_LOGW(TAG, "IMU unavailable; returning to standby");
                    (void)julia_fsm_runtime_post(EVT_IMU_UNAVAILABLE);
                }
            } else {
                imu_missing_since = 0;
                if (!voice_playback_is_active() && !board_audio_mic_is_enabled()) {
                    network_lifecycle_set_paused(true);
                }
            }
        } else {
            network_lifecycle_set_paused(false);
            if (state == JULIA_MAIN_STATE_S8_OTA) {
                recovering = true;
                set_capture(false);
            } else if (recovering &&
                       julia_fsm_runtime_get_service_state() == JULIA_SERVICE_ONLINE) {
                /* 恢复采音要求业务连接已回到 ONLINE，并以 MIC 实际使能作为完成判据：
                 * 只下发使能命令不算恢复成功。 */
                set_capture(true);
                if (board_audio_mic_is_enabled()) {
                    recovering = false;
                    ESP_LOGI(TAG, "network/session restored; voice wake ready");
                }
            }
        }
        was_quiet = quiet;
        /* 100 ms 轮询周期：旧路径没有状态通知驱动，只能周期采样状态与 IMU 可用性；
         * 该周期的来源未确认。 */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
#endif
}

/* 由 voice_service 的 FSM state observer 在每次状态变化后调用（见 voice_service_on_fsm_state）；
 * Task 尚未创建时静默忽略，不产生错误返回值。 */
void julia_quiet_power_notify(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

esp_err_t julia_quiet_power_init(void)
{
    if (s_task != NULL) return ESP_OK;
    return xTaskCreate(quiet_power_task, "quiet_power", 3072, NULL, 3, &s_task) == pdPASS
        ? ESP_OK : ESP_ERR_NO_MEM;
}

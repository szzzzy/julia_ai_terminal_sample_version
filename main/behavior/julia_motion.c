/**
 * @file julia_motion.c
 * @brief 在静默与待机状态观察设备是否被明显搬动，但不把桌面轻微振动当成用户唤醒。
 *
 * 连续多次出现足够大的加速度变化或转动后向行为 FSM 投递运动唤醒事件；扬声器
 * 播放和冷却期间暂停判断，避免设备自身振动重复触发。当前 capture-v1 配置下，
 * S3 与静默态（S5/S6）收到该事件后，运行时仅在业务链路和会话就绪时放行到 S4，
 * 否则丢弃事件并保持原状态，不在重连后补触发；只有关闭本地收音的
 * 兼容配置才把静默态恢复成 S3。本模块不直接操作显示，也不冒充唤醒词。
 * IMU 开机配置后保持采样关闭；本任务在进入/离开被监测状态时切换采样，
 * 重新开启后等待稳定并重建基线。关闭失败时继续重试，不把失败当作已省电。
 */
#include "julia_motion.h"

#include <math.h>
#include <stdbool.h>
#include <stdatomic.h>

static atomic_bool s_motion_ready;
bool julia_motion_ready(void) { return atomic_load(&s_motion_ready); }

#include "board_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "julia_fsm_runtime.h"
#include "qmi8658_shared.h"
#include "sdkconfig.h"

#if CONFIG_JULIA_IMU_MOTION_ENABLE

/* 任务栈与优先级是模块内常量，仓库里没有对应配置项，来源未确认。 */
#define MOTION_TASK_STACK_SIZE 3072
#define MOTION_TASK_PRIORITY   3
/* 门限来自 Kconfig：CONFIG_JULIA_IMU_ACCEL_DELTA_MG 是相邻样本三轴加速度变化绝对值之和
 * （单位 mg，换算见下方 MOTION_ACCEL_DELTA_G），CONFIG_JULIA_IMU_GYRO_THRESHOLD_DPS 是
 * 角速度模长（单位 dps）。取值以生效的 sdkconfig 为准，本注释不写死具体数字；
 * 仓库内没有标定记录，具体取值需要上板确认。 */
#define MOTION_ACCEL_DELTA_G \
    ((float)CONFIG_JULIA_IMU_ACCEL_DELTA_MG / 1000.0f)

static const char *TAG = "JULIA_MOTION";
static TaskHandle_t s_task;
/* 调用者是 FSM 状态观察者（voice_service_on_fsm_state，每次状态变化后执行），运行在任务
 * 上下文；实现使用 xTaskNotifyGive，因此不得从 ISR 调用。任务未创建时静默忽略。 */
void julia_motion_notify(void) { if (s_task) xTaskNotifyGive(s_task); }

/* 被监测状态集合：静默态 S5/S6 始终监测；capture-v1 起 S3 也监测，使待机设备能被
 * 搬动唤醒。关闭 CONFIG_JULIA_LOCAL_CAPTURE_ENABLE 时 S3 不采样，与旧的省电编排一致。 */
static bool motion_monitor_state(julia_main_state_t state)
{
    return julia_fsm_is_quiet(state)
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
        || state == JULIA_MAIN_STATE_S3_STANDBY
#endif
        ;
}

/* 采样开启/关闭只在状态跨越被监测集合时发生；失败时保持当前状态并重试，不把
 * "关不掉"当成已省电。重新开启后先等待传感器稳定（下一轮才建基线）。 */
static void motion_task(void *argument)
{
    (void)argument;
    board_imu_sample_t previous = {0};
    bool baseline_valid = false;
    bool sampling_enabled = false;
    unsigned consecutive = 0;
    TickType_t cooldown_until = 0;

    for (;;) {
        julia_main_state_t state = julia_fsm_runtime_get_state();
        TickType_t now = xTaskGetTickCount();
        bool want_sampling = motion_monitor_state(state);
        if (want_sampling != sampling_enabled) {
            baseline_valid = false;
            consecutive = 0;
            atomic_store(&s_motion_ready, false);
            esp_err_t err = board_imu_set_enabled(want_sampling);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "IMU sampling %s failed: %s; retrying",
                         want_sampling ? "enable" : "disable", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(CONFIG_JULIA_IMU_MOTION_SAMPLE_MS));
                continue;
            }
            sampling_enabled = want_sampling;
            ESP_LOGI(TAG, "IMU sampling %s", sampling_enabled ? "on (wake monitoring)" : "off");
            /* 重新开启后先等待传感器稳定；下一轮再确认状态并建立新基线。 */
            if (sampling_enabled) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
        }
        if (!motion_monitor_state(state)) {
            baseline_valid = false;
            consecutive = 0;
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_JULIA_IMU_MOTION_SAMPLE_MS));
        if (!motion_monitor_state(julia_fsm_runtime_get_state())) {
            baseline_valid = false;
            consecutive = 0;
            continue;
        }
        board_imu_sample_t current;
        if (board_imu_read(&current) != ESP_OK) {
            atomic_store(&s_motion_ready, false);
            baseline_valid = false;
            consecutive = 0;
            continue;
        }

        atomic_store(&s_motion_ready, true);
        /* 冷却/播放只暂停判断，仍读取以确认唤醒输入健康。 */
        if (board_audio_speaker_is_playing() ||
            (cooldown_until != 0 && now < cooldown_until)) {
            baseline_valid = false;
            consecutive = 0;
            continue;
        }
        if (!baseline_valid) {
            /* 首个可用样本只作为基线：确认窗口要求相邻样本比较，基线本身不参与判定。 */
            previous = current;
            baseline_valid = true;
            consecutive = 0;
            continue;
        }

        /* delta 是相邻样本三轴加速度变化绝对值之和（g）；gyro 是三轴角速度模长（dps）。 */
        float delta = fabsf(current.ax_g - previous.ax_g) +
                      fabsf(current.ay_g - previous.ay_g) +
                      fabsf(current.az_g - previous.az_g);
        float gyro = sqrtf(current.gx_dps * current.gx_dps +
                           current.gy_dps * current.gy_dps +
                           current.gz_dps * current.gz_dps);
        previous = current;

        bool detected = delta >= MOTION_ACCEL_DELTA_G ||
                        gyro >= (float)CONFIG_JULIA_IMU_GYRO_THRESHOLD_DPS;
        consecutive = detected ? consecutive + 1U : 0U;
        /* 必须连续 CONFIG_JULIA_IMU_MOTION_CONFIRM_FRAMES 帧都超过门限才确认，
         * 单帧尖峰或桌面轻碰不会触发唤醒。 */
        if (consecutive < CONFIG_JULIA_IMU_MOTION_CONFIRM_FRAMES) continue;

        esp_err_t post_err = julia_fsm_runtime_post(EVT_MOTION_WAKE);
        if (post_err == ESP_OK) {
            ESP_LOGI(TAG, "motion wake posted state=%s accel_delta=%.3fg gyro=%.1fdps",
                     julia_fsm_main_state_name(state), (double)delta, (double)gyro);
        } else {
            ESP_LOGW(TAG, "motion wake post failed: %s", esp_err_to_name(post_err));
        }
        consecutive = 0;
        baseline_valid = false;
        if (post_err == ESP_OK) {
            /* 冷却只在事件投递成功后开始计时：投递失败时下一轮仍可立即再次尝试。 */
            now = xTaskGetTickCount();
            cooldown_until = now + pdMS_TO_TICKS(CONFIG_JULIA_IMU_MOTION_COOLDOWN_MS);
        }
    }
}

/* 幂等：已创建任务直接返回 ESP_OK。board_imu_init() 在这里完成设备探测与寄存器配置，
 * 失败直接返回给启动流程，不创建任务。关闭 CONFIG_JULIA_IMU_MOTION_ENABLE 时本文件
 * 只剩空实现，init 返回 ESP_ERR_NOT_SUPPORTED。 */
esp_err_t julia_motion_init(void)
{
    if (s_task != NULL) return ESP_OK;
    esp_err_t err = board_imu_init();
    if (err != ESP_OK) return err;
    if (xTaskCreate(motion_task, "julia_motion", MOTION_TASK_STACK_SIZE, NULL,
                    MOTION_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    /* 日志里的 s6_to_s3 字段沿用旧命名，仅表示“运动唤醒输入已就绪”；capture-v1 下
     * 运动事件实际把 S3/S5/S6 送进 S4 发起交互，不能据此认为只回到 S3。 */
    ESP_LOGI(TAG, "ready s6_to_s3=1 sample=%dms confirm=%d accel=%.2fg gyro=%.1fdps",
             CONFIG_JULIA_IMU_MOTION_SAMPLE_MS,
             CONFIG_JULIA_IMU_MOTION_CONFIRM_FRAMES,
             (double)MOTION_ACCEL_DELTA_G,
             (double)CONFIG_JULIA_IMU_GYRO_THRESHOLD_DPS);
    return ESP_OK;
}
#else
void julia_motion_notify(void) { }
esp_err_t julia_motion_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

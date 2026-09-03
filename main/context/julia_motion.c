/**
 * @file julia_motion.c
 * @brief 设备睡眠时观察是否被明显搬动，但不把桌面轻微振动当成用户唤醒。
 *
 * 连续多次出现足够大的加速度变化或转动才记录一次运动；扬声器播放和冷却期间
 * 暂停判断，避免设备自身振动重复触发。运动目前只用于诊断，睡眠状态仍需唤醒词
 * 才能恢复交流。
 */
#include "julia_motion.h"

#include <math.h>
#include <stdbool.h>

#include "board_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "julia_fsm_runtime.h"
#include "qmi8658_shared.h"
#include "sdkconfig.h"

#define MOTION_TASK_STACK_SIZE 3072
#define MOTION_TASK_PRIORITY   3
#define MOTION_ACCEL_DELTA_G \
    ((float)CONFIG_JULIA_IMU_ACCEL_DELTA_MG / 1000.0f)

static const char *TAG = "JULIA_MOTION";
static TaskHandle_t s_task;

static bool motion_monitor_state(julia_main_state_t state)
{
    return state == JULIA_MAIN_STATE_S6_SLEEP;
}

static void motion_task(void *argument)
{
    (void)argument;
    board_imu_sample_t previous = {0};
    bool baseline_valid = false;
    unsigned consecutive = 0;
    TickType_t cooldown_until = 0;

    for (;;) {
        julia_main_state_t state = julia_fsm_runtime_get_state();
        TickType_t now = xTaskGetTickCount();
        if (!motion_monitor_state(state)) {
            baseline_valid = false;
            consecutive = 0;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (board_audio_speaker_is_playing() ||
            (cooldown_until != 0 && now < cooldown_until)) {
            baseline_valid = false;
            consecutive = 0;
            vTaskDelay(pdMS_TO_TICKS(CONFIG_JULIA_IMU_MOTION_SAMPLE_MS));
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
            baseline_valid = false;
            consecutive = 0;
            continue;
        }

        if (!baseline_valid) {
            previous = current;
            baseline_valid = true;
            consecutive = 0;
            continue;
        }

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
        if (consecutive < CONFIG_JULIA_IMU_MOTION_CONFIRM_FRAMES) continue;

        ESP_LOGI(TAG, "motion observed state=%s accel_delta=%.3fg gyro=%.1fdps; "
                      "S6 presentation retained",
                 julia_fsm_main_state_name(state), (double)delta, (double)gyro);
        consecutive = 0;
        baseline_valid = false;
        now = xTaskGetTickCount();
        cooldown_until = now + pdMS_TO_TICKS(CONFIG_JULIA_IMU_MOTION_COOLDOWN_MS);
    }
}

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
    ESP_LOGI(TAG, "ready s6_only=1 sample=%dms confirm=%d accel=%.2fg gyro=%.1fdps",
             CONFIG_JULIA_IMU_MOTION_SAMPLE_MS,
             CONFIG_JULIA_IMU_MOTION_CONFIRM_FRAMES,
             (double)MOTION_ACCEL_DELTA_G,
             (double)CONFIG_JULIA_IMU_GYRO_THRESHOLD_DPS);
    return ESP_OK;
}

/**
 * @file julia_motion.c
 * @brief QMI8658 短时运动检测与显示唤醒。
 *
 * 本模块比较相邻加速度样本与陀螺仪幅值，不执行姿态解算，也不依赖 SD。
 * 连续命中门限后只恢复显示；S3/S5/S6 进入 S4 仍必须由唤醒词触发。
 */
#include "julia_motion.h"

#include <math.h>
#include <stdbool.h>

#include "board_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "julia_fsm_runtime.h"
#include "julia_idle_display.h"
#include "qmi8658_shared.h"

#define MOTION_TASK_STACK_SIZE 3072
#define MOTION_TASK_PRIORITY   3
#define MOTION_SAMPLE_MS       100
#define MOTION_CONFIRM_FRAMES  8
#define MOTION_COOLDOWN_MS     10000
#define MOTION_ACCEL_DELTA_G   0.80f
#define MOTION_GYRO_DPS        120.0f

static const char *TAG = "JULIA_MOTION";
static TaskHandle_t s_task;

static bool motion_wake_state(julia_main_state_t state)
{
    return state == JULIA_MAIN_STATE_S6_SLEEP;
}

static void motion_task(void *argument)
{
    (void)argument;
    board_imu_sample_t previous = {0};
    if (board_imu_read(&previous) != ESP_OK) {
        ESP_LOGW(TAG, "initial baseline read failed");
    }
    unsigned consecutive = 0;
    TickType_t cooldown_until = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MOTION_SAMPLE_MS));
        board_imu_sample_t current;
        if (board_imu_read(&current) != ESP_OK) {
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

        julia_main_state_t state = julia_fsm_runtime_get_state();
        TickType_t now = xTaskGetTickCount();
        if (!motion_wake_state(state) || board_audio_speaker_is_playing() ||
            (cooldown_until != 0 && now < cooldown_until)) {
            consecutive = 0;
            continue;
        }

        bool detected = delta >= MOTION_ACCEL_DELTA_G || gyro >= MOTION_GYRO_DPS;
        consecutive = detected ? consecutive + 1U : 0U;
        if (consecutive < MOTION_CONFIRM_FRAMES) continue;

        ESP_LOGI(TAG, "motion display wake state=%s accel_delta=%.3fg gyro=%.1fdps",
                 julia_fsm_main_state_name(state), (double)delta, (double)gyro);
        julia_idle_display_note_activity();
        consecutive = 0;
        cooldown_until = now + pdMS_TO_TICKS(MOTION_COOLDOWN_MS);
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
    ESP_LOGI(TAG, "ready sample=%dms confirm=%d accel=%.2fg gyro=%.1fdps",
             MOTION_SAMPLE_MS, MOTION_CONFIRM_FRAMES,
             (double)MOTION_ACCEL_DELTA_G, (double)MOTION_GYRO_DPS);
    return ESP_OK;
}

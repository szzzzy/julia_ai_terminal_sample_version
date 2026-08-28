/**
 * @file qmi8658_shared.h
 * @brief Minimal QMI8658 accel/gyro access on the board shared I2C bus.
 */
#pragma once

#include "esp_err.h"

typedef struct {
    float ax_g;
    float ay_g;
    float az_g;
    float gx_dps;
    float gy_dps;
    float gz_dps;
} board_imu_sample_t;

/** Initialize QMI8658 at 30 Hz, +/-4 g and +/-64 dps. Idempotent. */
esp_err_t board_imu_init(void);

/** Read one converted accelerometer/gyroscope sample. */
esp_err_t board_imu_read(board_imu_sample_t *sample);

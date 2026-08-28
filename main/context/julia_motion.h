/**
 * @file julia_motion.h
 * @brief IMU motion wake source for far-standby and sleep states.
 */
#pragma once

#include "esp_err.h"

/** Initialize the shared QMI8658 and start motion detection. Idempotent. */
esp_err_t julia_motion_init(void);

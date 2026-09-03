/**
 * @file julia_motion.h
 * @brief S6 motion diagnostics; display wake remains owned by the FSM.
 */
#pragma once

#include "esp_err.h"

/** Initialize the shared QMI8658 and start motion detection. Idempotent. */
esp_err_t julia_motion_init(void);

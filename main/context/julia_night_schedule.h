/**
 * @file julia_night_schedule.h
 * @brief RTC-backed 23:00-07:00 night-sleep event source.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the wall-clock scheduler. Safe to call more than once. */
esp_err_t julia_night_schedule_init(void);

#ifdef __cplusplus
}
#endif

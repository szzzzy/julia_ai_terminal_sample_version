#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#define JULIA_SD_MOUNT_POINT "/sdcard"

esp_err_t julia_sd_init(bool format_if_mount_failed);
bool julia_sd_is_mounted(void);
bool julia_sd_lock(TickType_t timeout);
void julia_sd_unlock(void);
esp_err_t julia_sd_benchmark_read(float *average_mbps, float *minimum_mbps);

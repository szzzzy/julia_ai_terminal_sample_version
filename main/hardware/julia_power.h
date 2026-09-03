#pragma once

#include "esp_err.h"

/** Latch the board's battery power path on through BAT_Control (GPIO7). */
esp_err_t julia_power_hold_enable(void);

/** Enable 240/80 MHz dynamic frequency scaling without automatic light sleep. */
esp_err_t julia_power_management_init(void);

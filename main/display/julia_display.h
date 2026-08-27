#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** Initialize the board ST77916 panel and the LVGL display port. */
esp_err_t julia_display_init(void);

/** Keep the backlight dark until the first avatar frame is ready. */
esp_err_t julia_display_set_backlight(bool enabled);

bool julia_display_is_ready(void);

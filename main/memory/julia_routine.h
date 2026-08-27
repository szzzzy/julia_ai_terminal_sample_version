#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    JULIA_ACTIVITY_DIALOG = 0,
    JULIA_ACTIVITY_WAKE,
    JULIA_ACTIVITY_BUTTON,
    JULIA_ACTIVITY_SENSOR,
} activity_kind_t;

esp_err_t julia_routine_init(void);
void julia_routine_on_activity(activity_kind_t kind);
bool julia_routine_is_deviation(void);
esp_err_t julia_routine_flush(void);

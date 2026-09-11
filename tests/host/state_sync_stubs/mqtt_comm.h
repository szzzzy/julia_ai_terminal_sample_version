#pragma once
#include <stddef.h>
#include "esp_err.h"
esp_err_t mqtt_comm_publish_voice_status(const char *device_id, const char *data, size_t len);

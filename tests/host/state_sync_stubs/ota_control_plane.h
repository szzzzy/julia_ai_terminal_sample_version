#pragma once
#include <stddef.h>
#include "esp_err.h"
#define NATIVE_OTA_DEVICE_ID_SIZE 32
esp_err_t native_ota_get_device_id(char *device_id, size_t device_id_size);

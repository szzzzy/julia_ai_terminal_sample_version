#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Decode the project RGB565 RLE stream into exactly output_pixels pixels. */
esp_err_t avatar_rle_decode_rgb565(const uint8_t *input, size_t input_size,
                                   uint16_t *output, size_t output_pixels);

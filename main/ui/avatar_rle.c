#include "avatar_rle.h"

#include <string.h>

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

esp_err_t avatar_rle_decode_rgb565(const uint8_t *input, size_t input_size,
                                   uint16_t *output, size_t output_pixels)
{
    if (input == NULL || output == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t source = 0;
    size_t destination = 0;
    while (source + 2U <= input_size && destination < output_pixels) {
        uint16_t control = read_u16_le(input + source);
        source += 2U;
        size_t count = (size_t)(control & 0x7fffU) + 1U;
        if (count > output_pixels - destination) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (control & 0x8000U) {
            if (source + 2U > input_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            uint16_t pixel = read_u16_le(input + source);
            source += 2U;
            while (count-- > 0U) {
                output[destination++] = pixel;
            }
        } else {
            size_t bytes = count * sizeof(*output);
            if (source + bytes > input_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(output + destination, input + source, bytes);
            source += bytes;
            destination += count;
        }
    }

    return destination == output_pixels && source == input_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

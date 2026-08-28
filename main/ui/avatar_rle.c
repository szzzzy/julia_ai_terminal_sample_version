/**
 * @file    avatar_rle.c
 * @brief   项目自有 RGB565 RLE 解码器（供 julia_avatar 解相位帧）。
 *
 * 编码格式（小端 16bit）：
 *   控制字 control（LE uint16）分两段：
 *     - bit15 = 0（literal）：紧跟 count 个原始像素（count*2 字节），直接 memcpy。
 *     - bit15 = 1（run）：紧跟 1 个像素值，重复 count 次。
 *   count = (control & 0x7fff) + 1，因此取值范围 1..32768。
 * 输出严格为 output_pixels 个 uint16（360x360 RGB565）。
 *
 * 数据流：嵌入的 LISTEN/THINK/SPEAK bin → 本解码器 → PSRAM 360x360 帧 → 校验 CRC。
 *
 * 错误路径：参数为空(INVALID_ARG)、输出越界(INVALID_SIZE)、输入越界(INVALID_SIZE)、
 * 结尾尺寸不符(INVALID_SIZE)。任何错误即返回，不写越界内存。
 */
#include "avatar_rle.h"

#include <string.h>

/* 读一个 LE 16bit（不假设字节序/对齐）。 */
static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

/*
 * 契约：
 *   - input 为 RLE 流，input_size 为其字节数。
 *   - output 至少 output_pixels 项；函数精确写入 output_pixels 项。
 *   - 返回 ESP_OK 当且仅当 恰好解出 output_pixels 个像素且 input 被完全消费。
 */
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
        /* count 不能超出剩余输出空间。 */
        if (count > output_pixels - destination) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (control & 0x8000U) {
            /* run：复用一个像素 count 次。 */
            if (source + 2U > input_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            uint16_t pixel = read_u16_le(input + source);
            source += 2U;
            while (count-- > 0U) {
                output[destination++] = pixel;
            }
        } else {
            /* literal：原样拷贝 count 个像素（count*2 字节）。 */
            size_t bytes = count * sizeof(*output);
            if (source + bytes > input_size) {
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(output + destination, input + source, bytes);
            source += bytes;
            destination += count;
        }
    }

    /* 只有"恰好输出 output_pixels 且输入恰好消费完"才算成功。 */
    return destination == output_pixels && source == input_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

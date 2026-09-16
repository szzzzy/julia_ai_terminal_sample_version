/**
 * @file    avatar_rle.h
 * @brief   项目自有 RGB565 RLE 解码接口。
 *
 * 把 RLE 压缩的 360x360 RGB565 帧解到 output（output_pixels 项）。格式见 .c 文件头：
 * 每段一个 16bit 控制字，bit15 表 run/literal，低 15 位加 1 为长度。成功返回 ESP_OK。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** 把项目自有 RGB565 RLE 流解成恰好 output_pixels 个像素。
 *  不分配内存：output 缓冲由调用方准备，仅在输入被完全消费且输出恰好填满时成功。 */
esp_err_t avatar_rle_decode_rgb565(const uint8_t *input, size_t input_size,
                                   uint16_t *output, size_t output_pixels);

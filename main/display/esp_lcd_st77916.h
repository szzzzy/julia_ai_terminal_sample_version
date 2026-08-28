/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file    esp_lcd_st77916.h
 * @brief   ST77916 QSPI/SPI TFT LCD 面板驱动（实现 ESP-IDF esp_lcd_panel_t 接口）。
 *
 * @note  驱动职责与边界：
 *         - 本文件只声明面板驱动本身：`esp_lcd_new_panel_st77916()` 把一个 ST77916
 *           封装成通用 `esp_lcd_panel_t`（重置/初始化/开窗重绘/反显/镜像/坐标交换/开关显示）。
 *           它不碰 I2C 复位、不碰背光、不碰 LVGL——这些由上层 julia_display.c/lvgl_port.c 负责。
 *         - 支持的显示接口由 `st77916_vendor_config_t.flags.use_qspi_interface` 选择：
 *           QSPI（4 线数据，命令走 32-bit 命令字）或标准 SPI（DC 引脚区分命令/数据）。
 *           配套的初始化宏在本头下部，bus/IO 的引脚与时钟由调用方按需要填入。
 *         - 硬件前提：调用前必须已用 `esp_lcd_new_panel_io_spi()` 建好 `esp_lcd_panel_io_handle_t`
 *           （QSPI 时命令位宽 32、参数位宽 8、quad_mode=1），并已初始化对应 SPI 总线。
 *         - 分辨率不是驱动决定，而是由面板决定的（本板 360x360）；驱动只负责窗口与数据搬运。
 *
 * @note  初始化真实感：不同厂商/批次面板需要不同的寄存器初始化序列，因此驱动允许调用方通过
 *         `vendor_config.init_cmds` 覆盖厂商特定初始化命令；未提供时使用驱动内置默认序列。
 *
 * @see   main/display/julia_display.c（板级接线与初始化顺序）
 * @see   main/lvgl_port/lvgl_port.c（LVGL 刷新回调与 DMA 同步）
 */
#pragma once

#include <stdint.h>

#include "esp_lcd_panel_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LCD panel initialization commands.
 *
 */
typedef struct {
    int cmd;                /*<! The specific LCD command */
    const void *data;       /*<! Buffer that holds the command specific data */
    size_t data_bytes;      /*<! Size of `data` in memory, in bytes */
    unsigned int delay_ms;  /*<! Delay in milliseconds after this command */
} st77916_lcd_init_cmd_t;

/**
 * @brief LCD panel vendor configuration.
 *
 * @note  This structure can be used to select interface type and override default initialization commands.
 * @note  This structure needs to be passed to the `vendor_config` field in `esp_lcd_panel_dev_config_t`.
 *
 */
typedef struct {
    const st77916_lcd_init_cmd_t *init_cmds;    /*!< Pointer to initialization commands array.
                                                 *  The array should be declared as `static const` and positioned outside the function.
                                                 *  Please refer to `vendor_specific_init_default` in source file
                                                 */
    uint16_t init_cmds_size;    /*<! Number of commands in above array */
    struct {
        unsigned int use_qspi_interface: 1;     /*<! Set to 1 if use QSPI interface, default is SPI interface */
    } flags;
} st77916_vendor_config_t;

/**
 * @brief Create LCD panel for model ST77916
 *
 * @param[in]  io LCD panel IO handle
 * @param[in]  panel_dev_config General panel device configuration (Use `vendor_config` to select QSPI interface or override default initialization commands)
 * @param[out] ret_panel Returned LCD panel handle
 * @return
 *      - ESP_OK: Success
 *      - Otherwise: Fail
 */
esp_err_t esp_lcd_new_panel_st77916(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief 标准 SPI 总线配置宏（2 线：CLK + MOSI）。
 *
 * @note  MISO/QWP/QHD 置 -1：本面板为纯写模式，无需回读数据线。
 *        `max_trans_sz` 是单次 DMA 允许的最大传输字节数，调用方需按帧缓冲一行
 *        （或整屏）大小设置，超限时 esp_lcd_panel_io_spi 会自动分块。
 *
 * @param[in] sclk          SCK GPIO
 * @param[in] mosi          MOSI（数据输出）GPIO
 * @param[in] max_trans_sz  单次最大传输字节数
 */
#define ST77916_PANEL_BUS_SPI_CONFIG(sclk, mosi, max_trans_sz)  \
    {                                                           \
        .sclk_io_num = sclk,                                    \
        .mosi_io_num = mosi,                                    \
        .miso_io_num = -1,                                      \
        .quadhd_io_num = -1,                                    \
        .quadwp_io_num = -1,                                    \
        .max_transfer_sz = max_trans_sz,                        \
    }
/**
 * @brief QSPI 总线配置宏（4 线数据：D0-D3 + SCK）。
 *
 * @note  QSPI 模式下数据位宽为 4，带宽约为标准 SPI 的 4 倍，是全屏刷新不掉帧的关键。
 *        命令与参数以 32-bit 命令字发送（SPI IO 配置里 `lcd_cmd_bits=32`），
 *        数据以 8-bit 参数位宽发送，因此无需 DC 引脚区分命令/数据。
 *
 * @param[in] sclk          SCK GPIO
 * @param[in] d0,d1,d2,d3   四根数据线 GPIO
 * @param[in] max_trans_sz  单次最大传输字节数
 */
#define ST77916_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max_trans_sz)\
    {                                                           \
        .sclk_io_num = sclk,                                    \
        .data0_io_num = d0,                                     \
        .data1_io_num = d1,                                     \
        .data2_io_num = d2,                                     \
        .data3_io_num = d3,                                     \
        .max_transfer_sz = max_trans_sz,                        \
    }

/**
 * @brief 标准 SPI 的 panel IO 配置宏。
 *
 * @note  命令位宽 `lcd_cmd_bits=8`、参数位宽 `lcd_param_bits=8`：标准 SPI 用 DC 引脚区分
 *        命令（0）与数据（1），单字节即可。`on_color_trans_done` 是颜色数据发送完成的
 *        回调（在 DSPI ISR 上下文调用），用于让上层 flush 流程知道一帧 DMA 已结束。
 *
 * @param[in] cs     CS GPIO
 * @param[in] dc     DC（命令/数据）GPIO
 * @param[in] cb     on_color_trans_done 回调
 * @param[in] cb_ctx 回调 user_ctx
 */
#define ST77916_PANEL_IO_SPI_CONFIG(cs, dc, cb, cb_ctx)         \
    {                                                           \
        .cs_gpio_num = cs,                                      \
        .dc_gpio_num = dc,                                      \
        .spi_mode = 0,                                          \
        .pclk_hz = 40 * 1000 * 1000,                            \
        .trans_queue_depth = 10,                                \
        .on_color_trans_done = cb,                              \
        .user_ctx = cb_ctx,                                     \
        .lcd_cmd_bits = 8,                                      \
        .lcd_param_bits = 8,                                    \
    }
/**
 * @brief QSPI 的 panel IO 配置宏。
 *
 * @note  `dc_gpio_num=-1`、`lcd_cmd_bits=32`：QSPI 下命令经 32-bit 命令字注入，
 *        命令字里携带协议 opcode（写命令/读命令/写颜色），不再需要 DC 引脚。
 *        `quad_mode=1` 使数据走 4 线。`cb` 同样承载颜色传输完成回调。
 *
 * @param[in] cs     CS GPIO
 * @param[in] cb     on_color_trans_done 回调
 * @param[in] cb_ctx 回调 user_ctx
 */
#define ST77916_PANEL_IO_QSPI_CONFIG(cs, cb, cb_ctx)            \
    {                                                           \
        .cs_gpio_num = cs,                                      \
        .dc_gpio_num = -1,                                      \
        .spi_mode = 0,                                          \
        .pclk_hz = 40 * 1000 * 1000,                            \
        .trans_queue_depth = 10,                                \
        .on_color_trans_done = cb,                              \
        .user_ctx = cb_ctx,                                     \
        .lcd_cmd_bits = 32,                                     \
        .lcd_param_bits = 8,                                    \
        .flags = {                                              \
            .quad_mode = true,                                  \
        },                                                      \
    }

#ifdef __cplusplus
}
#endif

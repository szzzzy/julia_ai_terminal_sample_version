/**
 * @file    julia_display.h
 * @brief   板级显示封装接口：装配 ST77916(QSPI) 面板 + LVGL 显示端口。
 *
 * @note  本模块是"板级接线 + 初始化编排"层（见 julia_display.c）：初始化成功后即可
 *        通过 LVGL 渲染；背光需另由 julia_backlight 模块控制。
 * @see   main/lvgl_port/lvgl_port.h（LVGL 显示端口与刷新回调）
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** Initialize the board ST77916 panel and the LVGL display port. */
esp_err_t julia_display_init(void);

/** Keep the backlight dark until the first avatar frame is ready. */
esp_err_t julia_display_set_backlight(bool enabled);

bool julia_display_is_ready(void);

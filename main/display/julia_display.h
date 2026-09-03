/**
 * @file    julia_display.h
 * @brief   初始化板载 LCD，并在第一帧准备好之前保持背光关闭。
 *
 * @note  初始化成功只表示面板和绘图通道可用，不代表用户已经看到画面；应用应先
 *        绘制完整首帧，再打开背光，避免显示上电噪声或白屏。
 * @see   main/lvgl_port/lvgl_port.h（LVGL 显示端口与刷新回调）
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** 初始化 ST77916 面板及 LVGL 绘图通道。 */
esp_err_t julia_display_init(void);

/** 控制面板背光；应用启动时应在第一帧完整后再打开。 */
esp_err_t julia_display_set_backlight(bool enabled);

/** 查询 LCD 和绘图通道是否已经可以接收画面。 */
bool julia_display_is_ready(void);

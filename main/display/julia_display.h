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

/** 初始化 panel 与 LVGL port；失败可能留下部分资源，当前不支持反初始化后重试。 */
esp_err_t julia_display_init(void);

/** 当前没有实现体，现用背光接口为 julia_backlight；新代码不得调用该声明。 */
esp_err_t julia_display_set_backlight(bool enabled);

/** true 不代表首帧已完成或用户已经看到画面。 */
bool julia_display_is_ready(void);

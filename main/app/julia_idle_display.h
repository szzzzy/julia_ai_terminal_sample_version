/**
 * @file    julia_idle_display.h
 * @brief   记录用户最近是否仍在交流，并在陪伴窗口结束后请求返回待机。
 *
 * 职责边界：
 *   - 设备开机后默认等待唤醒；一次交流结束后保留一段无需再次说唤醒词的时间。
 *   - 用户正在说话、设备正在等待回答或正在播放回答时，不计算陪伴窗口超时。
 *   - 本模块只报告“陪伴窗口已经结束”，屏幕、背光和表情由设备状态统一控制，
 *     因此不会在睡眠状态生效后从旁路重新点亮屏幕。
 *
 * 依赖：FSM 运行时事件入口与 esp_timer。
 *
 * 使用：应用启动时开始计时；有效交流发生时刷新时间；听音、等待回答和播放回答
 *       期间声明设备仍在忙碌。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动陪伴窗口计时；初始状态为等待唤醒，不会重复报告用户离开。 */
esp_err_t julia_idle_display_init(void);

/** 记录一次有效交流，重新开始计算免唤醒陪伴时间。 */
void julia_idle_display_note_activity(void);

/** 声明设备是否正在听音、等待回答或播放回答；本函数不直接控制显示硬件。 */
void julia_idle_display_set_busy(bool busy);

/** 返回陪伴窗口是否已经结束；不代表显示面板或整机电源已经关闭。 */
bool julia_idle_display_is_sleeping(void);

#ifdef __cplusplus
}
#endif

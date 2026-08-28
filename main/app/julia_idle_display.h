/**
 * @file    julia_idle_display.h
 * @brief   Julia 待机显示策略：陪伴常驻，长时间无交互后进入闭眼呼吸。
 *
 * 职责边界：
 *   - 日常保持活跃陪伴显示；达到长休阈值后一次性切换到睡眠(闭眼+背光呼吸)，
 *     同时保证 LVGL/背光配合不打扰正在进行的
 *     对话播放。它只处理"屏幕该显示什么闲置状态"，不负责立绘内容本身（那是 julia_avatar/
 *     julia_ui 的职责）。
 *   - 注意与 FSM 的"设备行为状态"解耦：本模块只依据"交互活跃度 + busy 标志"，
 *     不直接从 FSM 读取主状态。
 *
 * 依赖：julia_backlight（背光呼吸/亮度）、julia_avatar（立绘 dozing 切换）、esp_timer。
 *
 * 使用：app_main 在 julia_avatar_init 成功后调用 julia_idle_display_init()；
 *       语音/交互处调用 note_activity()（用户交互）与 set_busy()（听-想-说期间保亮）。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the companion -> far-standby policy task. */
esp_err_t julia_idle_display_init(void);

/** Record a valid user or voice interaction and wake the display if needed. */
void julia_idle_display_note_activity(void);

/** Keep the display awake while a listen/think/speak operation is active. */
void julia_idle_display_set_busy(bool busy);

/** Return true after closed-eye breathing mode has started. */
bool julia_idle_display_is_sleeping(void);

#ifdef __cplusplus
}
#endif

/**
 * @file    julia_idle_display.h
 * @brief   Julia 待机显示策略：默认待机，交互后陪伴一段时间再回到闭眼呼吸。
 *
 * 职责边界：
 *   - 启动时跟随 FSM 的 S3 待机；有效交互后保持活跃陪伴显示，达到长休阈值后
 *     一次性切回待机（闭眼+背光呼吸），
 *     同时保证 LVGL/背光配合不打扰正在进行的
 *     对话播放。它只处理"屏幕该显示什么闲置状态"，不负责立绘内容本身（那是 julia_avatar/
 *     julia_ui 的职责）。
 *   - 注意与 FSM 的"设备行为状态"解耦：本模块只依据"交互活跃度 + busy 标志"
 *     投递事件，不直接控制状态对应的面板、背光或立绘。
 *
 * 依赖：FSM 运行时事件入口与 esp_timer。
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

/** Start the default-standby and post-interaction companion-window policy task. */
esp_err_t julia_idle_display_init(void);

/** Record valid activity; the resulting FSM state owns any display wake. */
void julia_idle_display_note_activity(void);

/** Prevent idle transition while listen/think/speak is active; does not drive hardware. */
void julia_idle_display_set_busy(bool busy);

/** Return true after closed-eye breathing mode has started. */
bool julia_idle_display_is_sleeping(void);

#ifdef __cplusplus
}
#endif

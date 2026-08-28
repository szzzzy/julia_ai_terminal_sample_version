#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief 语音输出与嘴型驱动的运行期耦合模式（临时测试开关）。
 *
 * FUSED：默认。同一 PCM 帧同时驱动嘴型动画并写入扬声器。
 * VOICE_ONLY：只播放声音，不驱动任何表情（用于单独测试语音链路）。
 * EXPR_ONLY：只驱动嘴型动画，不写扬声器；按真实帧节奏推进（用于单独测试表情链路）。
 */
typedef enum {
    JULIA_LIPSYNC_MODE_FUSED = 0,
    JULIA_LIPSYNC_MODE_VOICE_ONLY,
    JULIA_LIPSYNC_MODE_EXPR_ONLY,
} julia_lipsync_mode_t;

void julia_lipsync_set_mode(julia_lipsync_mode_t mode);
julia_lipsync_mode_t julia_lipsync_get_mode(void);
const char *julia_lipsync_mode_name(julia_lipsync_mode_t mode);

/**
 * @brief 当前模式是否会驱动表情 UI（FUSED / EXPR_ONLY 为 true，VOICE_ONLY 为 false）。
 *
 * 供语音链路在 VOICE_ONLY 时跳过对话相位等表情相关 UI 调用。
 */
bool julia_lipsync_ui_enabled(void);

/**
 * @brief 合成一段模拟语音的 PCM 包络并走完整的 begin/play/end 链路。
 *
 * 用于在不唤醒、不联网的情况下单独查看嘴型节奏；播放与否取决于当前模式。
 * duration_ms 为 0 时默认 3000ms。
 */
esp_err_t julia_lipsync_demo(uint32_t duration_ms);

/* ---- 会话生命周期 ----
 * 用法：julia_lipsync_begin() -> julia_lipsync_play()/play_file() 若干次 -> julia_lipsync_end()。
 * begin 清零累积状态并（FUSED/EXPR_ONLY）开启表情 UI 的 talking 状态；end 关闭 UI 与扬声器。
 * 未 begin 就 play 会导致状态未经初始化为 0，属未定义行为；重复 begin/end 按上述顺序复用。 */
void julia_lipsync_begin(void);
esp_err_t julia_lipsync_play(const int16_t *samples, size_t sample_count);
esp_err_t julia_lipsync_play_file(const char *path);
esp_err_t julia_lipsync_end(void);

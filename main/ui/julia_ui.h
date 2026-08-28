/**
 * @file    julia_ui.h
 * @brief   julia_ui.c 的公共接口声明（L0/L1 立绘 UI 总控）。
 *
 * 供调用方（julia_voice / julia_lipsync / julia_display_theme）使用的"状态->表情/画面"
 * 入口。多数函数要求 LVGL 已初始化（julia_ui_init）并且调用前无需自己加锁，
 * 函数内部会通过 lvgl_port_lock() 串行化。若模块未初始化或锁超时，函数返回/否则弃
 * （为 no-op），因此调用方不应依赖其返回的视觉结果。相关约定与线程模型详见 julia_ui.c。
 *
 * NOTE：需结合调用方确认——当前 main.c 的 L1 运行时链路走 julia_avatar（不进 julia_ui）；
 * 本头文件/实现是否仍被实际引用，决定其是保留还是裁剪。
 */
#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "esp_lcd_panel_ops.h"
#ifndef DOZE_FRAME_PATH
#define DOZE_FRAME_PATH "/sdcard/julia/doze_frame.bin"
#endif
#include "julia_fsm.h"

/* 表情枚举。注意：apply_expression 在立绘方案下直接 return，因此这些枚举目前
 * 只作"命名占位"，不再驱动可见的几何表情（见 julia_ui.c 的 apply_expression）。 */
typedef enum {
    JULIA_EXPR_SLEEP = 0,   ///< 睡眠表情。
    JULIA_EXPR_WATCHING,    ///< 注视表情。
    JULIA_EXPR_HAPPY,       ///< 开心表情。
    JULIA_EXPR_SPEAKING,    ///< 说话表情。
    JULIA_EXPR_CONFUSED,    ///< 困惑表情。
    JULIA_EXPR_COUNT,       ///< 计数器/越界判断。
} expr_t;
/* 对话框相位（IDLE/LISTENING/THINKING/SPEAKING），驱动嘴型与微动节奏。 */
typedef enum { JULIA_DIALOG_PHASE_IDLE=0, JULIA_DIALOG_PHASE_LISTENING, JULIA_DIALOG_PHASE_THINKING, JULIA_DIALOG_PHASE_SPEAKING } julia_dialog_phase_t;

void julia_ui_init(void);
void julia_ui_set_expression(expr_t expr, uint8_t intensity);
void julia_ui_set_state(julia_sub_state_t state);
void julia_ui_speak(const char *text);
void julia_ui_breathing_anim(void);
void julia_ui_talking_start(void);
void julia_ui_set_mouth_level(uint8_t level);
void julia_ui_set_mouth_openness(uint16_t openness_q8);
void julia_ui_talking_stop(void);
void julia_ui_set_dialog_phase(julia_dialog_phase_t phase);
void julia_ui_present_rgb565_frame(const uint16_t *pixels, size_t pixel_count);
void julia_ui_bind_rgb565_frame(uint16_t *pixels, size_t pixel_count);
void julia_ui_set_transition_frame_mode(bool enabled);
void julia_ui_crossfade_rgb565_frames(uint16_t *old_pixels, uint16_t *new_pixels,
                                      size_t pixel_count, uint8_t progress);
void julia_ui_apply_theme(uint32_t background_rgb, uint16_t transition_ms);
void julia_ui_set_program_blink_enabled(bool enabled);
void julia_ui_set_idle_frame_mode(bool enabled, julia_sub_state_t state);
esp_err_t julia_ui_transition_direct_begin(void);
esp_err_t julia_ui_transition_direct_draw(const uint16_t *pixels, size_t bytes,
                                          const char *source);
esp_err_t julia_ui_transition_direct_end(void);
void julia_ui_set_sleep_blackout(bool enabled);
void julia_ui_set_sleep_blackout_opa(uint8_t opacity);
void avatar_show_all(void);
/* Commit exactly one static frame while LVGL refresh is paused. */
esp_err_t julia_ui_draw_doze_frame(const char *path, bool *asset_loaded);
esp_err_t julia_ui_draw_standby_direct(esp_lcd_panel_handle_t panel);

/* Parent object reserved for replacing the placeholder with a real avatar. */
lv_obj_t *julia_ui_get_avatar_slot(void);
julia_sub_state_t julia_ui_current_state(void);

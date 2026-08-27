#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "esp_lcd_panel_ops.h"
#ifndef DOZE_FRAME_PATH
#define DOZE_FRAME_PATH "/sdcard/julia/doze_frame.bin"
#endif
#include "julia_fsm.h"

typedef enum {
    JULIA_EXPR_SLEEP = 0,
    JULIA_EXPR_WATCHING,
    JULIA_EXPR_HAPPY,
    JULIA_EXPR_SPEAKING,
    JULIA_EXPR_CONFUSED,
    JULIA_EXPR_COUNT,
} expr_t;
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

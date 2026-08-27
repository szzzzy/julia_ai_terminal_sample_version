#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "julia_fsm.h"

/* 可独立变换的现有 LVGL 图层。允许为 NULL，模块会安全跳过缺失图层。 */
typedef struct {
    lv_obj_t *container;
    lv_obj_t *neck;
    lv_obj_t *body;
    lv_obj_t *head;
    lv_obj_t *hair_front;
    lv_obj_t *hair_back;
    lv_obj_t *left_eye;
    lv_obj_t *right_eye;
    lv_obj_t *left_eyelid;
    lv_obj_t *right_eyelid;
    lv_obj_t *left_pupil;
    lv_obj_t *right_pupil;
    lv_obj_t *mouth;
} avatar_layer_bindings_t;

/* 泊松触发率使用“每分钟次数 x100”，避免配置表使用浮点数。 */
typedef struct {
    uint16_t blink_rate_x100;
    uint16_t wink_rate_x100;
    uint16_t mouth_rate_x100;
    uint16_t yawn_rate_x100;
    uint16_t far_gaze_rate_x100;
    uint16_t shoulder_rate_x100;
    uint16_t gaze_hold_min_ms;
    uint16_t gaze_hold_max_ms;
    uint16_t breath_period_ms;
    uint8_t breath_amplitude_px;
    uint8_t enabled;
} avatar_motion_config_t;

void avatar_micro_motion_init(const avatar_layer_bindings_t *layers);
void avatar_micro_motion_set_state(julia_sub_state_t state);
void avatar_micro_motion_set_dialog_phase(uint8_t phase);
void avatar_micro_motion_suspend(bool suspended);
const avatar_motion_config_t *avatar_micro_motion_config(julia_sub_state_t state);

/* 主循环每 50ms 调用。函数无动态内存、无文件 I/O，单帧计算为 O(1)。 */
void update_avatar(uint32_t now_ms);

/* 语音唤醒、触摸等用户事件调用，立即清零 boredom 并恢复正常姿态。 */
void on_user_interaction(void);

void avatar_breathe_init(lv_obj_t *container);
void avatar_head_init(lv_obj_t *container);
void avatar_neck_init(lv_obj_t *container);
void avatar_motion_pause(void);
void avatar_motion_resume(void);
void avatar_motion_pause_all(void);
void avatar_motion_resume_all(void);
bool avatar_motion_all_paused(void);
void avatar_motion_transition_main(julia_main_state_t state, uint16_t duration_ms);

#ifdef AVATAR_DEBUG
void avatar_motion_debug_trigger(uint8_t action);
void avatar_motion_debug_print(void);
void avatar_motion_debug_print_psram_peak(void);
#endif

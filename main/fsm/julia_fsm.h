/**
 * @file julia_fsm.h
 * @brief Julia 分层状态定义与事件入口。
 *
 * 第一层包含 S0～S8 共九个主状态。只有 S2 拥有第二层子状态：
 * S2.1 听、S2.2 思考、S2.3 说。当前主状态不是 S2 时，s2_sub_state
 * 必须为 JULIA_S2_SUB_STATE_NONE。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    JULIA_MAIN_STATE_S0_BOOT = 0,
    JULIA_MAIN_STATE_S1_COMPANION,
    JULIA_MAIN_STATE_S2_DIALOG,
    JULIA_MAIN_STATE_S3_STANDBY,
    JULIA_MAIN_STATE_S4_INTERACTION,
    JULIA_MAIN_STATE_S5_SILENT,
    JULIA_MAIN_STATE_S6_SLEEP,
    JULIA_MAIN_STATE_S7_FAULT,
    JULIA_MAIN_STATE_S8_OTA,
    JULIA_MAIN_STATE_COUNT,
} julia_main_state_t;

/** 仅归 JULIA_MAIN_STATE_S2_DIALOG 所有的子状态集合。 */
typedef enum {
    JULIA_S2_SUB_STATE_NONE = 0,
    JULIA_S2_SUB_STATE_S2_1_LISTENING,
    JULIA_S2_SUB_STATE_S2_2_THINKING,
    JULIA_S2_SUB_STATE_S2_3_SPEAKING,
    JULIA_S2_SUB_STATE_COUNT,
} julia_s2_sub_state_t;

/** 现有事件集合；听、想、说复用其中的语音事件，其余映射后续补充。 */
typedef enum {
    EVT_NONE = 0,
    EVT_USER_LEAVE,
    EVT_USER_RETURN,
    EVT_USER_CALL,
    EVT_EMOTION_DETECTED,
    EVT_ROUTINE_BREAK,
    EVT_SILENCE_TIMEOUT,
    EVT_USER_REJECT,
    EVT_USER_PERFUNCTORY,
    EVT_USER_LEFT_DIALOG,
    EVT_LOW_BATTERY,
    EVT_CHARGE_START,
    EVT_CHARGE_DONE,
    EVT_NIGHT_TIME,
    EVT_MANUAL_SLEEP,
    EVT_DAY_AWAY,
    EVT_BEDTIME,
    EVT_SHARED_ACTIVITY_START,
    EVT_SHARED_ACTIVITY_STOP,
    EVT_START_DIALOG,
    EVT_DEEP_TALK_DETECTED,
    EVT_MULTI_TURN_DETECTED,
    EVT_INTERRUPT,
    EVT_RECOVERY_ATTEMPT,
    EVT_WAKEUP,
    EVT_BOOT_COMPLETE,
    EVT_SYSTEM_FAULT,
    EVT_COUNT,
} fsm_event_t;

typedef struct julia_fsm julia_fsm_t;
typedef void (*julia_fsm_state_cb_t)(julia_fsm_t *fsm,
                                     julia_main_state_t main_state,
                                     julia_s2_sub_state_t s2_sub_state,
                                     fsm_event_t event);

struct julia_fsm {
    julia_main_state_t main_state;
    julia_s2_sub_state_t s2_sub_state;
    julia_fsm_state_cb_t on_enter;
    julia_fsm_state_cb_t on_exit;
    void *user_ctx;
};

void julia_fsm_init(julia_fsm_t *fsm);
bool julia_fsm_handle_event(julia_fsm_t *fsm, fsm_event_t event, void *data);
bool julia_fsm_state_is_valid(julia_main_state_t main_state,
                              julia_s2_sub_state_t s2_sub_state);
bool julia_fsm_can_transition(julia_main_state_t from_main_state,
                              julia_s2_sub_state_t from_s2_sub_state,
                              julia_main_state_t to_main_state,
                              julia_s2_sub_state_t to_s2_sub_state);
bool julia_fsm_transition_to(julia_fsm_t *fsm,
                             julia_main_state_t to_main_state,
                             julia_s2_sub_state_t to_s2_sub_state,
                             fsm_event_t reason);
const char *julia_fsm_main_state_name(julia_main_state_t state);
const char *julia_fsm_s2_sub_state_name(julia_s2_sub_state_t state);
const char *julia_fsm_event_name(fsm_event_t event);

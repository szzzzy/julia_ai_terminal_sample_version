#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    JULIA_MAIN_STATE_S0_SLEEP = 0,
    JULIA_MAIN_STATE_S1_STANDBY,
    JULIA_MAIN_STATE_S2_COMPANION,
    JULIA_MAIN_STATE_S3_INITIATIVE,
    JULIA_MAIN_STATE_S4_DIALOG,
    JULIA_MAIN_STATE_S5_SILENT,
    JULIA_MAIN_STATE_COUNT,
} julia_main_state_t;

typedef enum {
    JULIA_SUB_STATE_S0_1_NIGHT_SLEEP = 0,
    JULIA_SUB_STATE_S0_2_DAY_AWAY,
    JULIA_SUB_STATE_S0_3_MANUAL_SLEEP,
    JULIA_SUB_STATE_S1_1_NEAR_STANDBY,
    JULIA_SUB_STATE_S1_2_FAR_STANDBY,
    JULIA_SUB_STATE_S1_3_CHARGING_STANDBY,
    JULIA_SUB_STATE_S2_1_OBSERVE,
    JULIA_SUB_STATE_S2_2_SHARED_ACTIVITY,
    JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION,
    JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER,
    JULIA_SUB_STATE_S3_2_ROUTINE_BREAK,
    JULIA_SUB_STATE_S3_3_USER_CALL,
    JULIA_SUB_STATE_S3_4_RECOVERY_PROBE,
    JULIA_SUB_STATE_S4_1_LIGHT_DIALOG,
    JULIA_SUB_STATE_S4_2_DEEP_TALK,
    JULIA_SUB_STATE_S4_3_MULTI_TURN,
    JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE,
    JULIA_SUB_STATE_S5_1_USER_REJECT,
    JULIA_SUB_STATE_S5_2_USER_PERFUNCTORY,
    JULIA_SUB_STATE_S5_3_USER_LEFT,
    JULIA_SUB_STATE_COUNT,
} julia_sub_state_t;

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
} fsm_event_t;

typedef struct julia_fsm julia_fsm_t;
typedef void (*julia_fsm_state_cb_t)(julia_fsm_t *fsm, julia_sub_state_t state, fsm_event_t evt);

struct julia_fsm {
    julia_main_state_t main_state;
    julia_sub_state_t sub_state;
    julia_fsm_state_cb_t on_enter;
    julia_fsm_state_cb_t on_exit;
    void *user_ctx;
};

void julia_fsm_init(julia_fsm_t *fsm);
bool julia_fsm_handle_event(julia_fsm_t *fsm, fsm_event_t evt, void *data);
const char *julia_fsm_main_state_name(julia_main_state_t state);
const char *julia_fsm_sub_state_name(julia_sub_state_t state);
const char *julia_fsm_event_name(fsm_event_t evt);

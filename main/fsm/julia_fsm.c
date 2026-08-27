#include "julia_fsm.h"

#include <stdio.h>
#include "esp_log.h"


#define TAG "JULIA_FSM"

typedef struct {
    julia_main_state_t main_state;
    const char *name;
} julia_sub_state_info_t;

typedef julia_sub_state_t (*julia_state_handler_t)(julia_fsm_t *fsm, fsm_event_t evt, void *data);

static const julia_sub_state_info_t s_sub_state_info[JULIA_SUB_STATE_COUNT] = {
    [JULIA_SUB_STATE_S0_1_NIGHT_SLEEP] = {JULIA_MAIN_STATE_S0_SLEEP, "S0.1"},
    [JULIA_SUB_STATE_S0_2_DAY_AWAY] = {JULIA_MAIN_STATE_S0_SLEEP, "S0.2"},
    [JULIA_SUB_STATE_S0_3_MANUAL_SLEEP] = {JULIA_MAIN_STATE_S0_SLEEP, "S0.3"},
    [JULIA_SUB_STATE_S1_1_NEAR_STANDBY] = {JULIA_MAIN_STATE_S1_STANDBY, "S1.1"},
    [JULIA_SUB_STATE_S1_2_FAR_STANDBY] = {JULIA_MAIN_STATE_S1_STANDBY, "S1.2"},
    [JULIA_SUB_STATE_S1_3_CHARGING_STANDBY] = {JULIA_MAIN_STATE_S1_STANDBY, "S1.3"},
    [JULIA_SUB_STATE_S2_1_OBSERVE] = {JULIA_MAIN_STATE_S2_COMPANION, "S2.1"},
    [JULIA_SUB_STATE_S2_2_SHARED_ACTIVITY] = {JULIA_MAIN_STATE_S2_COMPANION, "S2.2"},
    [JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION] = {JULIA_MAIN_STATE_S2_COMPANION, "S2.3"},
    [JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER] = {JULIA_MAIN_STATE_S3_INITIATIVE, "S3.1"},
    [JULIA_SUB_STATE_S3_2_ROUTINE_BREAK] = {JULIA_MAIN_STATE_S3_INITIATIVE, "S3.2"},
    [JULIA_SUB_STATE_S3_3_USER_CALL] = {JULIA_MAIN_STATE_S3_INITIATIVE, "S3.3"},
    [JULIA_SUB_STATE_S3_4_RECOVERY_PROBE] = {JULIA_MAIN_STATE_S3_INITIATIVE, "S3.4"},
    [JULIA_SUB_STATE_S4_1_LIGHT_DIALOG] = {JULIA_MAIN_STATE_S4_DIALOG, "S4.1"},
    [JULIA_SUB_STATE_S4_2_DEEP_TALK] = {JULIA_MAIN_STATE_S4_DIALOG, "S4.2"},
    [JULIA_SUB_STATE_S4_3_MULTI_TURN] = {JULIA_MAIN_STATE_S4_DIALOG, "S4.3"},
    [JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE] = {JULIA_MAIN_STATE_S4_DIALOG, "S4.4"},
    [JULIA_SUB_STATE_S5_1_USER_REJECT] = {JULIA_MAIN_STATE_S5_SILENT, "S5.1"},
    [JULIA_SUB_STATE_S5_2_USER_PERFUNCTORY] = {JULIA_MAIN_STATE_S5_SILENT, "S5.2"},
    [JULIA_SUB_STATE_S5_3_USER_LEFT] = {JULIA_MAIN_STATE_S5_SILENT, "S5.3"},
};

static const char *s_main_state_names[JULIA_MAIN_STATE_COUNT] = {
    [JULIA_MAIN_STATE_S0_SLEEP] = "S0",
    [JULIA_MAIN_STATE_S1_STANDBY] = "S1",
    [JULIA_MAIN_STATE_S2_COMPANION] = "S2",
    [JULIA_MAIN_STATE_S3_INITIATIVE] = "S3",
    [JULIA_MAIN_STATE_S4_DIALOG] = "S4",
    [JULIA_MAIN_STATE_S5_SILENT] = "S5",
};

static const char *s_event_names[] = {
    [EVT_NONE] = "EVT_NONE",
    [EVT_USER_LEAVE] = "EVT_USER_LEAVE",
    [EVT_USER_RETURN] = "EVT_USER_RETURN",
    [EVT_USER_CALL] = "EVT_USER_CALL",
    [EVT_EMOTION_DETECTED] = "EVT_EMOTION_DETECTED",
    [EVT_ROUTINE_BREAK] = "EVT_ROUTINE_BREAK",
    [EVT_SILENCE_TIMEOUT] = "EVT_SILENCE_TIMEOUT",
    [EVT_USER_REJECT] = "EVT_USER_REJECT",
    [EVT_USER_PERFUNCTORY] = "EVT_USER_PERFUNCTORY",
    [EVT_USER_LEFT_DIALOG] = "EVT_USER_LEFT_DIALOG",
    [EVT_LOW_BATTERY] = "EVT_LOW_BATTERY",
    [EVT_CHARGE_START] = "EVT_CHARGE_START",
    [EVT_CHARGE_DONE] = "EVT_CHARGE_DONE",
    [EVT_NIGHT_TIME] = "EVT_NIGHT_TIME",
    [EVT_MANUAL_SLEEP] = "EVT_MANUAL_SLEEP",
    [EVT_DAY_AWAY] = "EVT_DAY_AWAY",
    [EVT_BEDTIME] = "EVT_BEDTIME",
    [EVT_SHARED_ACTIVITY_START] = "EVT_SHARED_ACTIVITY_START",
    [EVT_SHARED_ACTIVITY_STOP] = "EVT_SHARED_ACTIVITY_STOP",
    [EVT_START_DIALOG] = "EVT_START_DIALOG",
    [EVT_DEEP_TALK_DETECTED] = "EVT_DEEP_TALK_DETECTED",
    [EVT_MULTI_TURN_DETECTED] = "EVT_MULTI_TURN_DETECTED",
    [EVT_INTERRUPT] = "EVT_INTERRUPT",
    [EVT_RECOVERY_ATTEMPT] = "EVT_RECOVERY_ATTEMPT",
    [EVT_WAKEUP] = "EVT_WAKEUP",
};

static julia_sub_state_t handle_state_s0_1(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s0_2(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s0_3(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s1_1(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s1_2(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s1_3(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s2_1(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s2_2(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s2_3(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s3_1(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s3_2(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s3_3(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s3_4(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s4_1(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s4_2(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s4_3(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s4_4(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s5_1(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s5_2(julia_fsm_t *fsm, fsm_event_t evt, void *data);
static julia_sub_state_t handle_state_s5_3(julia_fsm_t *fsm, fsm_event_t evt, void *data);

static const julia_state_handler_t s_state_handlers[JULIA_SUB_STATE_COUNT] = {
    [JULIA_SUB_STATE_S0_1_NIGHT_SLEEP] = handle_state_s0_1,
    [JULIA_SUB_STATE_S0_2_DAY_AWAY] = handle_state_s0_2,
    [JULIA_SUB_STATE_S0_3_MANUAL_SLEEP] = handle_state_s0_3,
    [JULIA_SUB_STATE_S1_1_NEAR_STANDBY] = handle_state_s1_1,
    [JULIA_SUB_STATE_S1_2_FAR_STANDBY] = handle_state_s1_2,
    [JULIA_SUB_STATE_S1_3_CHARGING_STANDBY] = handle_state_s1_3,
    [JULIA_SUB_STATE_S2_1_OBSERVE] = handle_state_s2_1,
    [JULIA_SUB_STATE_S2_2_SHARED_ACTIVITY] = handle_state_s2_2,
    [JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION] = handle_state_s2_3,
    [JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER] = handle_state_s3_1,
    [JULIA_SUB_STATE_S3_2_ROUTINE_BREAK] = handle_state_s3_2,
    [JULIA_SUB_STATE_S3_3_USER_CALL] = handle_state_s3_3,
    [JULIA_SUB_STATE_S3_4_RECOVERY_PROBE] = handle_state_s3_4,
    [JULIA_SUB_STATE_S4_1_LIGHT_DIALOG] = handle_state_s4_1,
    [JULIA_SUB_STATE_S4_2_DEEP_TALK] = handle_state_s4_2,
    [JULIA_SUB_STATE_S4_3_MULTI_TURN] = handle_state_s4_3,
    [JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE] = handle_state_s4_4,
    [JULIA_SUB_STATE_S5_1_USER_REJECT] = handle_state_s5_1,
    [JULIA_SUB_STATE_S5_2_USER_PERFUNCTORY] = handle_state_s5_2,
    [JULIA_SUB_STATE_S5_3_USER_LEFT] = handle_state_s5_3,
};

static void default_on_enter(julia_fsm_t *fsm, julia_sub_state_t state, fsm_event_t evt)
{
    (void)fsm;
    ESP_LOGI(TAG, "enter %s by %s", julia_fsm_sub_state_name(state), julia_fsm_event_name(evt));
}

static void default_on_exit(julia_fsm_t *fsm, julia_sub_state_t state, fsm_event_t evt)
{
    (void)fsm;
    ESP_LOGI(TAG, "exit %s by %s", julia_fsm_sub_state_name(state), julia_fsm_event_name(evt));
}

static void julia_fsm_transition(julia_fsm_t *fsm, julia_sub_state_t next_state, fsm_event_t evt)
{
    julia_sub_state_t prev_state = fsm->sub_state;
    julia_main_state_t prev_main_state = fsm->main_state;

    if (prev_state == next_state) {
        return;
    }

    if (fsm->on_exit != NULL) {
        fsm->on_exit(fsm, prev_state, evt);
    }

    fsm->sub_state = next_state;
    fsm->main_state = s_sub_state_info[next_state].main_state;

    if (fsm->main_state != prev_main_state) {
        char summary[16];
        snprintf(summary, sizeof(summary), "S%d->S%d", prev_main_state, fsm->main_state);
    }

    ESP_LOGI(TAG, "[FSM] %s -> %s (原因: %s)",
             julia_fsm_sub_state_name(prev_state),
             julia_fsm_sub_state_name(next_state),
             julia_fsm_event_name(evt));

    if (fsm->on_enter != NULL) {
        fsm->on_enter(fsm, next_state, evt);
    }
}

static julia_sub_state_t handle_global_event(julia_fsm_t *fsm, fsm_event_t evt)
{
    (void)fsm;

    switch (evt) {
    case EVT_EMOTION_DETECTED:
        return JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER;
    case EVT_CHARGE_START:
        return JULIA_SUB_STATE_S1_3_CHARGING_STANDBY;
    case EVT_CHARGE_DONE:
    case EVT_WAKEUP:
        return JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
    case EVT_LOW_BATTERY:
    case EVT_MANUAL_SLEEP:
        return JULIA_SUB_STATE_S0_3_MANUAL_SLEEP;
    case EVT_NIGHT_TIME:
        return JULIA_SUB_STATE_S0_1_NIGHT_SLEEP;
    case EVT_DAY_AWAY:
        return JULIA_SUB_STATE_S0_2_DAY_AWAY;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

static julia_sub_state_t handle_sleep_common(fsm_event_t evt)
{
    switch (evt) {
    case EVT_USER_RETURN:
    case EVT_USER_CALL:
    case EVT_WAKEUP:
        return JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

static julia_sub_state_t handle_initiative_common(fsm_event_t evt)
{
    switch (evt) {
    case EVT_START_DIALOG:
        return JULIA_SUB_STATE_S4_1_LIGHT_DIALOG;
    case EVT_USER_REJECT:
        return JULIA_SUB_STATE_S5_1_USER_REJECT;
    case EVT_USER_PERFUNCTORY:
        return JULIA_SUB_STATE_S5_2_USER_PERFUNCTORY;
    case EVT_SILENCE_TIMEOUT:
        return JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

static julia_sub_state_t handle_rejection_common(fsm_event_t evt)
{
    switch (evt) {
    case EVT_SILENCE_TIMEOUT:
        return JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
    case EVT_RECOVERY_ATTEMPT:
    case EVT_USER_CALL:
        return JULIA_SUB_STATE_S3_4_RECOVERY_PROBE;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

static julia_sub_state_t handle_dialog_common(fsm_event_t evt)
{
    switch (evt) {
    case EVT_USER_LEFT_DIALOG:
        return JULIA_SUB_STATE_S5_3_USER_LEFT;
    case EVT_USER_REJECT:
        return JULIA_SUB_STATE_S5_1_USER_REJECT;
    case EVT_USER_PERFUNCTORY:
        return JULIA_SUB_STATE_S5_2_USER_PERFUNCTORY;
    case EVT_SILENCE_TIMEOUT:
        return JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

static julia_sub_state_t handle_state_s0_1(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_sleep_common(evt);
}

static julia_sub_state_t handle_state_s0_2(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_sleep_common(evt);
}

static julia_sub_state_t handle_state_s0_3(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_sleep_common(evt);
}

static julia_sub_state_t handle_state_s1_1(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_USER_LEAVE:
        return JULIA_SUB_STATE_S1_2_FAR_STANDBY;
    case EVT_EMOTION_DETECTED:
        return JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER;
    case EVT_ROUTINE_BREAK:
        return JULIA_SUB_STATE_S3_2_ROUTINE_BREAK;
    case EVT_USER_CALL:
        return JULIA_SUB_STATE_S3_3_USER_CALL;
    case EVT_BEDTIME:
        return JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION;
    case EVT_SHARED_ACTIVITY_START:
        return JULIA_SUB_STATE_S2_2_SHARED_ACTIVITY;
    case EVT_SILENCE_TIMEOUT:
        return JULIA_SUB_STATE_S2_1_OBSERVE;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

static julia_sub_state_t handle_state_s1_2(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_USER_RETURN:
        return JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
    case EVT_RECOVERY_ATTEMPT:
        return JULIA_SUB_STATE_S3_4_RECOVERY_PROBE;
    case EVT_SILENCE_TIMEOUT:
        return JULIA_SUB_STATE_S2_1_OBSERVE;
    case EVT_USER_CALL:
        return JULIA_SUB_STATE_S3_3_USER_CALL;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

static julia_sub_state_t handle_state_s1_3(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_USER_CALL:
        return JULIA_SUB_STATE_S3_3_USER_CALL;
    case EVT_USER_RETURN:
        return JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

static julia_sub_state_t handle_state_s2_1(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_SHARED_ACTIVITY_START:
        return JULIA_SUB_STATE_S2_2_SHARED_ACTIVITY;
    case EVT_BEDTIME:
        return JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION;
    case EVT_USER_CALL:
        return JULIA_SUB_STATE_S3_3_USER_CALL;
    case EVT_USER_LEAVE:
        return JULIA_SUB_STATE_S1_2_FAR_STANDBY;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

static julia_sub_state_t handle_state_s2_2(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_SHARED_ACTIVITY_STOP:
        return JULIA_SUB_STATE_S2_1_OBSERVE;
    case EVT_START_DIALOG:
        return JULIA_SUB_STATE_S4_3_MULTI_TURN;
    case EVT_USER_LEAVE:
        return JULIA_SUB_STATE_S1_2_FAR_STANDBY;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

static julia_sub_state_t handle_state_s2_3(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_SILENCE_TIMEOUT:
        return JULIA_SUB_STATE_S0_1_NIGHT_SLEEP;
    case EVT_USER_CALL:
    case EVT_START_DIALOG:
        return JULIA_SUB_STATE_S4_1_LIGHT_DIALOG;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

static julia_sub_state_t handle_state_s3_1(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_initiative_common(evt);
}

static julia_sub_state_t handle_state_s3_2(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_initiative_common(evt);
}

static julia_sub_state_t handle_state_s3_3(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_initiative_common(evt);
}

static julia_sub_state_t handle_state_s3_4(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_initiative_common(evt);
}

static julia_sub_state_t handle_state_s4_1(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_DEEP_TALK_DETECTED:
        return JULIA_SUB_STATE_S4_2_DEEP_TALK;
    case EVT_MULTI_TURN_DETECTED:
        return JULIA_SUB_STATE_S4_3_MULTI_TURN;
    case EVT_INTERRUPT:
        return JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE;
    default:
        return handle_dialog_common(evt);
    }
}

static julia_sub_state_t handle_state_s4_2(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_MULTI_TURN_DETECTED:
        return JULIA_SUB_STATE_S4_3_MULTI_TURN;
    case EVT_INTERRUPT:
        return JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE;
    default:
        return handle_dialog_common(evt);
    }
}

static julia_sub_state_t handle_state_s4_3(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_INTERRUPT:
        return JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE;
    default:
        return handle_dialog_common(evt);
    }
}

static julia_sub_state_t handle_state_s4_4(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;

    switch (evt) {
    case EVT_START_DIALOG:
        return JULIA_SUB_STATE_S4_1_LIGHT_DIALOG;
    default:
        return handle_dialog_common(evt);
    }
}

static julia_sub_state_t handle_state_s5_1(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_rejection_common(evt);
}

static julia_sub_state_t handle_state_s5_2(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    return handle_rejection_common(evt);
}

static julia_sub_state_t handle_state_s5_3(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    (void)fsm;
    (void)data;
    switch (evt) {
    case EVT_USER_RETURN:
        /* The dialogue owner can use the saved conversation history to resume. */
        return JULIA_SUB_STATE_S4_1_LIGHT_DIALOG;
    case EVT_SILENCE_TIMEOUT:
        return JULIA_SUB_STATE_S1_2_FAR_STANDBY;
    default:
        return JULIA_SUB_STATE_COUNT;
    }
}

const char *julia_fsm_main_state_name(julia_main_state_t state)
{
    if (state >= JULIA_MAIN_STATE_COUNT || s_main_state_names[state] == NULL) {
        return "UNKNOWN_MAIN";
    }
    return s_main_state_names[state];
}

const char *julia_fsm_sub_state_name(julia_sub_state_t state)
{
    if (state >= JULIA_SUB_STATE_COUNT || s_sub_state_info[state].name == NULL) {
        return "UNKNOWN_SUB";
    }
    return s_sub_state_info[state].name;
}

const char *julia_fsm_event_name(fsm_event_t evt)
{
    if ((size_t)evt >= sizeof(s_event_names) / sizeof(s_event_names[0]) || s_event_names[evt] == NULL) {
        return "UNKNOWN_EVT";
    }
    return s_event_names[evt];
}

void julia_fsm_init(julia_fsm_t *fsm)
{
    if (fsm == NULL) {
        return;
    }

    fsm->main_state = JULIA_MAIN_STATE_S1_STANDBY;
    fsm->sub_state = JULIA_SUB_STATE_S1_1_NEAR_STANDBY;
    fsm->on_enter = default_on_enter;
    fsm->on_exit = default_on_exit;
    fsm->user_ctx = NULL;

    if (fsm->on_enter != NULL) {
        fsm->on_enter(fsm, fsm->sub_state, EVT_NONE);
    }
}

bool julia_fsm_handle_event(julia_fsm_t *fsm, fsm_event_t evt, void *data)
{
    if (fsm == NULL || fsm->sub_state >= JULIA_SUB_STATE_COUNT) {
        return false;
    }

    julia_sub_state_t next_state = handle_global_event(fsm, evt);
    if (next_state == JULIA_SUB_STATE_COUNT) {
        julia_state_handler_t handler = s_state_handlers[fsm->sub_state];
        if (handler == NULL) {
            ESP_LOGW(TAG, "[FSM] no handler for state %s", julia_fsm_sub_state_name(fsm->sub_state));
            return false;
        }
        next_state = handler(fsm, evt, data);
    }

    if (next_state < JULIA_SUB_STATE_COUNT) {
        julia_fsm_transition(fsm, next_state, evt);
        return true;
    }

    ESP_LOGI(TAG, "[FSM] %s ignored event %s",
             julia_fsm_sub_state_name(fsm->sub_state),
             julia_fsm_event_name(evt));
    return false;
}

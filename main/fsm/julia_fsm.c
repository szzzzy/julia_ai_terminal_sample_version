/**
 * @file julia_fsm.c
 * @brief S0～S8 分层状态的元数据与允许迁移图。
 *
 * 本文件负责校验并执行调用方明确请求的状态切换。听、想、说阶段复用
 * 现有语音事件完成映射，其他事件到目标状态的规则将在后续补充。
 */
#include "julia_fsm.h"

#include <stddef.h>

#include "esp_log.h"

#define TAG "JULIA_FSM"

static const char *const s_main_state_names[JULIA_MAIN_STATE_COUNT] = {
    [JULIA_MAIN_STATE_S0_BOOT] = "S0_BOOT",
    [JULIA_MAIN_STATE_S1_COMPANION] = "S1_COMPANION",
    [JULIA_MAIN_STATE_S2_DIALOG] = "S2_DIALOG",
    [JULIA_MAIN_STATE_S3_STANDBY] = "S3_STANDBY",
    [JULIA_MAIN_STATE_S4_INTERACTION] = "S4_INTERACTION",
    [JULIA_MAIN_STATE_S5_SILENT] = "S5_SILENT",
    [JULIA_MAIN_STATE_S6_SLEEP] = "S6_SLEEP",
    [JULIA_MAIN_STATE_S7_FAULT] = "S7_FAULT",
    [JULIA_MAIN_STATE_S8_OTA] = "S8_OTA",
};

static const char *const s_s2_sub_state_names[JULIA_S2_SUB_STATE_COUNT] = {
    [JULIA_S2_SUB_STATE_NONE] = "NONE",
    [JULIA_S2_SUB_STATE_S2_1_LISTENING] = "S2.1_LISTENING",
    [JULIA_S2_SUB_STATE_S2_2_THINKING] = "S2.2_THINKING",
    [JULIA_S2_SUB_STATE_S2_3_SPEAKING] = "S2.3_SPEAKING",
};

static const char *const s_event_names[EVT_COUNT] = {
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
    [EVT_BOOT_COMPLETE] = "EVT_BOOT_COMPLETE",
    [EVT_SYSTEM_FAULT] = "EVT_SYSTEM_FAULT",
};

static void default_on_enter(julia_fsm_t *fsm, julia_main_state_t main_state,
                             julia_s2_sub_state_t s2_sub_state, fsm_event_t event)
{
    (void)fsm;
    ESP_LOGI(TAG, "enter %s/%s by %s", julia_fsm_main_state_name(main_state),
             julia_fsm_s2_sub_state_name(s2_sub_state),
             julia_fsm_event_name(event));
}

static void default_on_exit(julia_fsm_t *fsm, julia_main_state_t main_state,
                            julia_s2_sub_state_t s2_sub_state, fsm_event_t event)
{
    (void)fsm;
    ESP_LOGI(TAG, "exit %s/%s by %s", julia_fsm_main_state_name(main_state),
             julia_fsm_s2_sub_state_name(s2_sub_state),
             julia_fsm_event_name(event));
}

bool julia_fsm_state_is_valid(julia_main_state_t main_state,
                              julia_s2_sub_state_t s2_sub_state)
{
    if (main_state >= JULIA_MAIN_STATE_COUNT ||
        s2_sub_state >= JULIA_S2_SUB_STATE_COUNT) return false;
    if (main_state == JULIA_MAIN_STATE_S2_DIALOG) {
        return s2_sub_state != JULIA_S2_SUB_STATE_NONE;
    }
    return s2_sub_state == JULIA_S2_SUB_STATE_NONE;
}

static bool target_is(julia_main_state_t to_main_state,
                      julia_s2_sub_state_t to_s2_sub_state,
                      julia_main_state_t expected_main_state)
{
    return to_main_state == expected_main_state &&
           to_s2_sub_state == JULIA_S2_SUB_STATE_NONE;
}

bool julia_fsm_can_transition(julia_main_state_t from_main_state,
                              julia_s2_sub_state_t from_s2_sub_state,
                              julia_main_state_t to_main_state,
                              julia_s2_sub_state_t to_s2_sub_state)
{
    if (!julia_fsm_state_is_valid(from_main_state, from_s2_sub_state) ||
        !julia_fsm_state_is_valid(to_main_state, to_s2_sub_state) ||
        (from_main_state == to_main_state &&
         from_s2_sub_state == to_s2_sub_state)) return false;

    /* 除 S7 自身外，所有状态都可以进入 S7；S7 唯一允许的出口是 S0。 */
    if (from_main_state == JULIA_MAIN_STATE_S7_FAULT) {
        return target_is(to_main_state, to_s2_sub_state,
                         JULIA_MAIN_STATE_S0_BOOT);
    }
    if (target_is(to_main_state, to_s2_sub_state,
                  JULIA_MAIN_STATE_S7_FAULT)) return true;

    switch (from_main_state) {
    case JULIA_MAIN_STATE_S0_BOOT:
        return target_is(to_main_state, to_s2_sub_state,
                         JULIA_MAIN_STATE_S1_COMPANION) ||
               target_is(to_main_state, to_s2_sub_state,
                         JULIA_MAIN_STATE_S8_OTA);

    case JULIA_MAIN_STATE_S1_COMPANION:
        return (to_main_state == JULIA_MAIN_STATE_S2_DIALOG &&
                to_s2_sub_state == JULIA_S2_SUB_STATE_S2_1_LISTENING) ||
               target_is(to_main_state, to_s2_sub_state,
                         JULIA_MAIN_STATE_S3_STANDBY) ||
               target_is(to_main_state, to_s2_sub_state,
                         JULIA_MAIN_STATE_S8_OTA);

    case JULIA_MAIN_STATE_S2_DIALOG:
        if (from_s2_sub_state == JULIA_S2_SUB_STATE_S2_3_SPEAKING &&
            target_is(to_main_state, to_s2_sub_state,
                      JULIA_MAIN_STATE_S1_COMPANION)) return true;
        if (to_main_state != JULIA_MAIN_STATE_S2_DIALOG) return false;
        return (from_s2_sub_state == JULIA_S2_SUB_STATE_S2_1_LISTENING &&
                to_s2_sub_state == JULIA_S2_SUB_STATE_S2_2_THINKING) ||
               (from_s2_sub_state == JULIA_S2_SUB_STATE_S2_2_THINKING &&
                to_s2_sub_state == JULIA_S2_SUB_STATE_S2_3_SPEAKING) ||
               (from_s2_sub_state == JULIA_S2_SUB_STATE_S2_3_SPEAKING &&
                to_s2_sub_state == JULIA_S2_SUB_STATE_S2_1_LISTENING);

    case JULIA_MAIN_STATE_S3_STANDBY:
        return target_is(to_main_state, to_s2_sub_state,
                         JULIA_MAIN_STATE_S4_INTERACTION) ||
               target_is(to_main_state, to_s2_sub_state,
                         JULIA_MAIN_STATE_S6_SLEEP);

    case JULIA_MAIN_STATE_S4_INTERACTION:
        return target_is(to_main_state, to_s2_sub_state,
                         JULIA_MAIN_STATE_S5_SILENT) ||
               (to_main_state == JULIA_MAIN_STATE_S2_DIALOG &&
                to_s2_sub_state == JULIA_S2_SUB_STATE_S2_1_LISTENING);

    case JULIA_MAIN_STATE_S5_SILENT:
        return target_is(to_main_state, to_s2_sub_state,
                         JULIA_MAIN_STATE_S4_INTERACTION) ||
               target_is(to_main_state, to_s2_sub_state,
                         JULIA_MAIN_STATE_S3_STANDBY);

    case JULIA_MAIN_STATE_S6_SLEEP:
        return target_is(to_main_state, to_s2_sub_state,
                         JULIA_MAIN_STATE_S4_INTERACTION);

    case JULIA_MAIN_STATE_S8_OTA:
    case JULIA_MAIN_STATE_S7_FAULT:
    case JULIA_MAIN_STATE_COUNT:
    default:
        return false;
    }
}

bool julia_fsm_transition_to(julia_fsm_t *fsm,
                             julia_main_state_t to_main_state,
                             julia_s2_sub_state_t to_s2_sub_state,
                             fsm_event_t reason)
{
    if (fsm == NULL || reason >= EVT_COUNT ||
        !julia_fsm_can_transition(fsm->main_state, fsm->s2_sub_state,
                                  to_main_state, to_s2_sub_state)) return false;

    julia_main_state_t from_main_state = fsm->main_state;
    julia_s2_sub_state_t from_s2_sub_state = fsm->s2_sub_state;
    if (fsm->on_exit != NULL) {
        fsm->on_exit(fsm, from_main_state, from_s2_sub_state, reason);
    }
    fsm->main_state = to_main_state;
    fsm->s2_sub_state = to_s2_sub_state;
    ESP_LOGI(TAG, "[FSM] %s/%s -> %s/%s (%s)",
             julia_fsm_main_state_name(from_main_state),
             julia_fsm_s2_sub_state_name(from_s2_sub_state),
             julia_fsm_main_state_name(to_main_state),
             julia_fsm_s2_sub_state_name(to_s2_sub_state),
             julia_fsm_event_name(reason));
    if (fsm->on_enter != NULL) {
        fsm->on_enter(fsm, to_main_state, to_s2_sub_state, reason);
    }
    return true;
}

void julia_fsm_init(julia_fsm_t *fsm)
{
    if (fsm == NULL) return;
    fsm->main_state = JULIA_MAIN_STATE_S0_BOOT;
    fsm->s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    fsm->on_enter = default_on_enter;
    fsm->on_exit = default_on_exit;
    fsm->user_ctx = NULL;
    fsm->on_enter(fsm, fsm->main_state, fsm->s2_sub_state, EVT_NONE);
}

bool julia_fsm_handle_event(julia_fsm_t *fsm, fsm_event_t event, void *data)
{
    (void)data;
    if (fsm == NULL || !julia_fsm_state_is_valid(fsm->main_state, fsm->s2_sub_state) ||
        event <= EVT_NONE || event >= EVT_COUNT) return false;

    julia_main_state_t target_main_state = JULIA_MAIN_STATE_COUNT;
    julia_s2_sub_state_t target_s2_sub_state = JULIA_S2_SUB_STATE_COUNT;

    if (event == EVT_SYSTEM_FAULT &&
        fsm->main_state != JULIA_MAIN_STATE_S7_FAULT) {
        /* 故障入口对 S0～S6、S8 全局有效；进入 S7 后由运行时立即复位。 */
        target_main_state = JULIA_MAIN_STATE_S7_FAULT;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if (fsm->main_state == JULIA_MAIN_STATE_S0_BOOT &&
               event == EVT_BOOT_COMPLETE) {
        /* 本地关键服务初始化完成后，开机态进入陪伴态。 */
        target_main_state = JULIA_MAIN_STATE_S1_COMPANION;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    /* 复用现有语音链路事件驱动“听 -> 想 -> 说”，不新增阶段事件。 */
    } else if (event == EVT_USER_CALL &&
        (fsm->main_state == JULIA_MAIN_STATE_S1_COMPANION ||
         fsm->main_state == JULIA_MAIN_STATE_S4_INTERACTION)) {
        target_main_state = JULIA_MAIN_STATE_S2_DIALOG;
        target_s2_sub_state = JULIA_S2_SUB_STATE_S2_1_LISTENING;
    } else if (fsm->main_state == JULIA_MAIN_STATE_S2_DIALOG) {
        if (fsm->s2_sub_state == JULIA_S2_SUB_STATE_S2_1_LISTENING &&
            event == EVT_START_DIALOG) {
            target_main_state = JULIA_MAIN_STATE_S2_DIALOG;
            target_s2_sub_state = JULIA_S2_SUB_STATE_S2_2_THINKING;
        } else if (fsm->s2_sub_state == JULIA_S2_SUB_STATE_S2_2_THINKING &&
                   event == EVT_MULTI_TURN_DETECTED) {
            target_main_state = JULIA_MAIN_STATE_S2_DIALOG;
            target_s2_sub_state = JULIA_S2_SUB_STATE_S2_3_SPEAKING;
        } else if (fsm->s2_sub_state == JULIA_S2_SUB_STATE_S2_3_SPEAKING &&
                   (event == EVT_INTERRUPT || event == EVT_USER_CALL)) {
            target_main_state = JULIA_MAIN_STATE_S2_DIALOG;
            target_s2_sub_state = JULIA_S2_SUB_STATE_S2_1_LISTENING;
        } else if (fsm->s2_sub_state == JULIA_S2_SUB_STATE_S2_3_SPEAKING &&
                   event == EVT_SILENCE_TIMEOUT) {
            target_main_state = JULIA_MAIN_STATE_S1_COMPANION;
            target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
        }
    }

    if (target_main_state < JULIA_MAIN_STATE_COUNT) {
        return julia_fsm_transition_to(fsm, target_main_state,
                                       target_s2_sub_state, event);
    }

    ESP_LOGI(TAG, "[FSM] 事件尚未映射：%s，当前状态=%s/%s",
             julia_fsm_event_name(event),
             julia_fsm_main_state_name(fsm->main_state),
             julia_fsm_s2_sub_state_name(fsm->s2_sub_state));
    return false;
}

const char *julia_fsm_main_state_name(julia_main_state_t state)
{
    return state < JULIA_MAIN_STATE_COUNT && s_main_state_names[state] != NULL
               ? s_main_state_names[state] : "UNKNOWN_MAIN";
}

const char *julia_fsm_s2_sub_state_name(julia_s2_sub_state_t state)
{
    return state < JULIA_S2_SUB_STATE_COUNT && s_s2_sub_state_names[state] != NULL
               ? s_s2_sub_state_names[state] : "UNKNOWN_S2_SUB";
}

const char *julia_fsm_event_name(fsm_event_t event)
{
    return event >= EVT_NONE && event < EVT_COUNT && s_event_names[event] != NULL
               ? s_event_names[event] : "UNKNOWN_EVT";
}

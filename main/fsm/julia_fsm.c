/**
 * @file julia_fsm.c
 * @brief Julia 行为状态机的状态校验、允许迁移图与现有事件映射。
 *
 * 允许迁移图负责拒绝非法跳转；事件入口只映射当前工程已经实际产生的
 * 空闲、唤醒、夜间和语音事件。未实现的故障、OTA 等业务不在这里预留处理分支。
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
    [EVT_USER_CALL] = "EVT_USER_CALL",
    [EVT_SILENCE_TIMEOUT] = "EVT_SILENCE_TIMEOUT",
    [EVT_NIGHT_TIME] = "EVT_NIGHT_TIME",
    [EVT_STANDBY_TIMEOUT] = "EVT_STANDBY_TIMEOUT",
    [EVT_SILENT_TIMEOUT] = "EVT_SILENT_TIMEOUT",
    [EVT_BEDTIME] = "EVT_BEDTIME",
    [EVT_START_DIALOG] = "EVT_START_DIALOG",
    [EVT_MULTI_TURN_DETECTED] = "EVT_MULTI_TURN_DETECTED",
    [EVT_INTERRUPT] = "EVT_INTERRUPT",
    [EVT_WAKEUP] = "EVT_WAKEUP",
    [EVT_INTENT_GOODNIGHT] = "EVT_INTENT_GOODNIGHT",
    [EVT_INTENT_DISMISS] = "EVT_INTENT_DISMISS",
    [EVT_OTA_AVAILABLE] = "EVT_OTA_AVAILABLE",
    [EVT_OTA_SUCCEEDED] = "EVT_OTA_SUCCEEDED",
    [EVT_OTA_TASK_FAILED] = "EVT_OTA_TASK_FAILED",
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
        /* 服务端特殊语义在听/想阶段结束当前轮次：晚安进入 S6，结束沟通进入 S5。
         * 说话阶段不接受迟到语义，避免状态已睡眠但旧回答仍在播放。 */
        if (from_s2_sub_state != JULIA_S2_SUB_STATE_S2_3_SPEAKING &&
            (target_is(to_main_state, to_s2_sub_state,
                       JULIA_MAIN_STATE_S5_SILENT) ||
             target_is(to_main_state, to_s2_sub_state,
                       JULIA_MAIN_STATE_S6_SLEEP))) return true;
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
               target_is(to_main_state, to_s2_sub_state,
                         JULIA_MAIN_STATE_S6_SLEEP) ||
               (to_main_state == JULIA_MAIN_STATE_S2_DIALOG &&
                to_s2_sub_state == JULIA_S2_SUB_STATE_S2_2_THINKING);

    case JULIA_MAIN_STATE_S5_SILENT:
        return target_is(to_main_state, to_s2_sub_state,
                         JULIA_MAIN_STATE_S4_INTERACTION) ||
               target_is(to_main_state, to_s2_sub_state,
                         JULIA_MAIN_STATE_S3_STANDBY);

    case JULIA_MAIN_STATE_S6_SLEEP:
        return target_is(to_main_state, to_s2_sub_state,
                         JULIA_MAIN_STATE_S4_INTERACTION);

    case JULIA_MAIN_STATE_S8_OTA:
        return target_is(to_main_state, to_s2_sub_state,
                         JULIA_MAIN_STATE_S0_BOOT) ||
               target_is(to_main_state, to_s2_sub_state,
                         JULIA_MAIN_STATE_S1_COMPANION);
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

    if (fsm->main_state == JULIA_MAIN_STATE_S1_COMPANION &&
               event == EVT_USER_LEAVE) {
        /* 复用显示空闲策略的用户离开事件，由陪伴态进入待机态。 */
        target_main_state = JULIA_MAIN_STATE_S3_STANDBY;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if ((fsm->main_state == JULIA_MAIN_STATE_S0_BOOT ||
                fsm->main_state == JULIA_MAIN_STATE_S1_COMPANION) &&
               event == EVT_OTA_AVAILABLE) {
        target_main_state = JULIA_MAIN_STATE_S8_OTA;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if (fsm->main_state == JULIA_MAIN_STATE_S3_STANDBY &&
               event == EVT_WAKEUP) {
        /* 只有语音链路确认的唤醒词能从待机进入发起交互态。 */
        target_main_state = JULIA_MAIN_STATE_S4_INTERACTION;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if (fsm->main_state == JULIA_MAIN_STATE_S3_STANDBY &&
               (event == EVT_NIGHT_TIME || event == EVT_STANDBY_TIMEOUT)) {
        /* 夜间窗口或 S3 驻留超时都进入睡眠态。 */
        target_main_state = JULIA_MAIN_STATE_S6_SLEEP;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if ((fsm->main_state == JULIA_MAIN_STATE_S5_SILENT ||
                fsm->main_state == JULIA_MAIN_STATE_S6_SLEEP) &&
               event == EVT_WAKEUP) {
        /* 静默态和睡眠态同样只响应唤醒词进入发起交互态。 */
        target_main_state = JULIA_MAIN_STATE_S4_INTERACTION;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if (fsm->main_state == JULIA_MAIN_STATE_S4_INTERACTION &&
               event == EVT_START_DIALOG) {
        /* 正常话语结束直接进入“想”，不要求服务端额外返回 dialog 意图。 */
        target_main_state = JULIA_MAIN_STATE_S2_DIALOG;
        target_s2_sub_state = JULIA_S2_SUB_STATE_S2_2_THINKING;
    } else if ((fsm->main_state == JULIA_MAIN_STATE_S4_INTERACTION ||
                (fsm->main_state == JULIA_MAIN_STATE_S2_DIALOG &&
                 fsm->s2_sub_state != JULIA_S2_SUB_STATE_S2_3_SPEAKING)) &&
               event == EVT_INTENT_GOODNIGHT) {
        /* “晚安”结束本轮沟通并立即进入睡眠，不再绕经 S5/S3 计时。 */
        target_main_state = JULIA_MAIN_STATE_S6_SLEEP;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if ((fsm->main_state == JULIA_MAIN_STATE_S4_INTERACTION ||
                (fsm->main_state == JULIA_MAIN_STATE_S2_DIALOG &&
                 fsm->s2_sub_state != JULIA_S2_SUB_STATE_S2_3_SPEAKING)) &&
               event == EVT_INTENT_DISMISS) {
        /* 明确结束沟通进入静默，仍按 S5 的独立计时策略返回 S3。 */
        target_main_state = JULIA_MAIN_STATE_S5_SILENT;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if (fsm->main_state == JULIA_MAIN_STATE_S5_SILENT &&
               event == EVT_SILENT_TIMEOUT) {
        target_main_state = JULIA_MAIN_STATE_S3_STANDBY;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if (fsm->main_state == JULIA_MAIN_STATE_S8_OTA &&
               event == EVT_OTA_SUCCEEDED) {
        target_main_state = JULIA_MAIN_STATE_S0_BOOT;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if (fsm->main_state == JULIA_MAIN_STATE_S8_OTA &&
               event == EVT_OTA_TASK_FAILED) {
        target_main_state = JULIA_MAIN_STATE_S1_COMPANION;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    /* 复用现有语音链路事件驱动“听 -> 想 -> 说”，不新增阶段事件。 */
    } else if (event == EVT_USER_CALL &&
               fsm->main_state == JULIA_MAIN_STATE_S1_COMPANION) {
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

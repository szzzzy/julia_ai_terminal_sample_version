/**
 * @file julia_fsm.c
 * @brief 定义设备在开机、待机、对话、睡眠、故障和升级之间如何切换。
 *
 * 每个外部事件先按当前业务状态确定去向，再由统一入口执行变化。这样可以拒绝
 * 不符合产品流程的跳转，例如设备尚未被唤醒时直接进入播放回答。
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

static const char *const s_s7_sub_state_names[JULIA_S7_SUB_STATE_COUNT] = {
    [JULIA_S7_SUB_STATE_NONE] = "NONE",
    [JULIA_S7_SUB_STATE_S7_1_DISCONNECTED] = "S7.1_DISCONNECTED",
    [JULIA_S7_SUB_STATE_S7_2_FAULT] = "S7.2_FAULT",
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
    [EVT_MQTT_DISCONNECTED] = "EVT_MQTT_DISCONNECTED",
    [EVT_WSS_DISCONNECTED] = "EVT_WSS_DISCONNECTED",
    [EVT_DISCONNECT_NOTICE_TIMEOUT] = "EVT_DISCONNECT_NOTICE_TIMEOUT",
    [EVT_OTA_AVAILABLE] = "EVT_OTA_AVAILABLE",
    [EVT_OTA_SUCCEEDED] = "EVT_OTA_SUCCEEDED",
    [EVT_OTA_TASK_FAILED] = "EVT_OTA_TASK_FAILED",
};

static void default_on_enter(julia_fsm_t *fsm, julia_main_state_t main_state,
                             julia_s2_sub_state_t s2_sub_state, fsm_event_t event)
{
    (void)fsm;
    ESP_LOGI(TAG, "enter %s/%s/%s by %s", julia_fsm_main_state_name(main_state),
             julia_fsm_s2_sub_state_name(s2_sub_state),
             julia_fsm_s7_sub_state_name(fsm->s7_sub_state),
             julia_fsm_event_name(event));
}

static void default_on_exit(julia_fsm_t *fsm, julia_main_state_t main_state,
                            julia_s2_sub_state_t s2_sub_state, fsm_event_t event)
{
    (void)fsm;
    ESP_LOGI(TAG, "exit %s/%s/%s by %s", julia_fsm_main_state_name(main_state),
             julia_fsm_s2_sub_state_name(s2_sub_state),
             julia_fsm_s7_sub_state_name(fsm->s7_sub_state),
             julia_fsm_event_name(event));
}

bool julia_fsm_state_is_valid(julia_main_state_t main_state,
                              julia_s2_sub_state_t s2_sub_state)
{
    return julia_fsm_state_is_valid_full(main_state, s2_sub_state,
        main_state == JULIA_MAIN_STATE_S7_FAULT
            ? JULIA_S7_SUB_STATE_S7_2_FAULT
            : JULIA_S7_SUB_STATE_NONE);
}

bool julia_fsm_state_is_valid_full(julia_main_state_t main_state,
                                   julia_s2_sub_state_t s2_sub_state,
                                   julia_s7_sub_state_t s7_sub_state)
{
    if (main_state >= JULIA_MAIN_STATE_COUNT ||
        s2_sub_state >= JULIA_S2_SUB_STATE_COUNT ||
        s7_sub_state >= JULIA_S7_SUB_STATE_COUNT) return false;
    if (main_state == JULIA_MAIN_STATE_S2_DIALOG) {
        return s2_sub_state != JULIA_S2_SUB_STATE_NONE &&
               s7_sub_state == JULIA_S7_SUB_STATE_NONE;
    }
    if (main_state == JULIA_MAIN_STATE_S7_FAULT) {
        return s2_sub_state == JULIA_S2_SUB_STATE_NONE &&
               s7_sub_state != JULIA_S7_SUB_STATE_NONE;
    }
    return s2_sub_state == JULIA_S2_SUB_STATE_NONE &&
           s7_sub_state == JULIA_S7_SUB_STATE_NONE;
}

static bool target_is(julia_main_state_t to_main_state,
                      julia_s2_sub_state_t to_s2_sub_state,
                      julia_s7_sub_state_t to_s7_sub_state,
                      julia_main_state_t expected_main_state)
{
    return to_main_state == expected_main_state &&
           to_s2_sub_state == JULIA_S2_SUB_STATE_NONE &&
           to_s7_sub_state == JULIA_S7_SUB_STATE_NONE;
}

static bool target_is_disconnected(julia_main_state_t to_main_state,
                                   julia_s2_sub_state_t to_s2_sub_state,
                                   julia_s7_sub_state_t to_s7_sub_state)
{
    return to_main_state == JULIA_MAIN_STATE_S7_FAULT &&
           to_s2_sub_state == JULIA_S2_SUB_STATE_NONE &&
           to_s7_sub_state == JULIA_S7_SUB_STATE_S7_1_DISCONNECTED;
}

static bool target_is_fault(julia_main_state_t to_main_state,
                            julia_s2_sub_state_t to_s2_sub_state,
                            julia_s7_sub_state_t to_s7_sub_state)
{
    return to_main_state == JULIA_MAIN_STATE_S7_FAULT &&
           to_s2_sub_state == JULIA_S2_SUB_STATE_NONE &&
           to_s7_sub_state == JULIA_S7_SUB_STATE_S7_2_FAULT;
}

bool julia_fsm_can_transition_full(julia_main_state_t from_main_state,
                                   julia_s2_sub_state_t from_s2_sub_state,
                                   julia_s7_sub_state_t from_s7_sub_state,
                                   julia_main_state_t to_main_state,
                                   julia_s2_sub_state_t to_s2_sub_state,
                                   julia_s7_sub_state_t to_s7_sub_state)
{
    if (!julia_fsm_state_is_valid_full(from_main_state, from_s2_sub_state,
                                       from_s7_sub_state) ||
        !julia_fsm_state_is_valid_full(to_main_state, to_s2_sub_state,
                                       to_s7_sub_state) ||
        (from_main_state == to_main_state &&
         from_s2_sub_state == to_s2_sub_state &&
         from_s7_sub_state == to_s7_sub_state)) return false;

    /* S7.2 严重故障只能通过复位重新开机；S7.1 是可恢复提示，显示结束后进入待机。
     * 如果提示期间又发生真正的核心故障，则升级为 S7.2 并走故障记录与复位。 */
    if (from_main_state == JULIA_MAIN_STATE_S7_FAULT) {
        if (from_s7_sub_state == JULIA_S7_SUB_STATE_S7_1_DISCONNECTED) {
            return target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                             JULIA_MAIN_STATE_S3_STANDBY) ||
                   target_is_fault(to_main_state, to_s2_sub_state,
                                   to_s7_sub_state);
        }
        return target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S0_BOOT);
    }
    if (target_is_fault(to_main_state, to_s2_sub_state, to_s7_sub_state)) return true;
    if (target_is_disconnected(to_main_state, to_s2_sub_state, to_s7_sub_state)) {
        /* 只有依赖 WSS/MQTT 才能继续的交流状态需要显示断联提示。已经待机、睡眠、
         * 静默或升级中的设备不会因重复断线通知而改变用户已经选择的状态。 */
        return from_main_state == JULIA_MAIN_STATE_S1_COMPANION ||
               from_main_state == JULIA_MAIN_STATE_S2_DIALOG ||
               from_main_state == JULIA_MAIN_STATE_S4_INTERACTION;
    }

    switch (from_main_state) {
    case JULIA_MAIN_STATE_S0_BOOT:
        return target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S3_STANDBY) ||
               target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S8_OTA);

    case JULIA_MAIN_STATE_S1_COMPANION:
        return (to_main_state == JULIA_MAIN_STATE_S2_DIALOG &&
                to_s2_sub_state == JULIA_S2_SUB_STATE_S2_1_LISTENING) ||
               target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S3_STANDBY) ||
               target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S8_OTA);

    case JULIA_MAIN_STATE_S2_DIALOG:
        if (from_s2_sub_state == JULIA_S2_SUB_STATE_S2_3_SPEAKING &&
            target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                      JULIA_MAIN_STATE_S1_COMPANION)) return true;
        /* 用户的“晚安”或“结束交流”可能在回答音频前后到达，因此听音、等待回答
         * 和播放回答阶段都允许直接结束本轮交流。语音服务会先停止尚未播完的声音。 */
        if (target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                      JULIA_MAIN_STATE_S5_SILENT) ||
            target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                      JULIA_MAIN_STATE_S6_SLEEP) ||
            target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                      JULIA_MAIN_STATE_S3_STANDBY)) return true;
        if (to_main_state != JULIA_MAIN_STATE_S2_DIALOG) return false;
        return (from_s2_sub_state == JULIA_S2_SUB_STATE_S2_1_LISTENING &&
                to_s2_sub_state == JULIA_S2_SUB_STATE_S2_2_THINKING) ||
               (from_s2_sub_state == JULIA_S2_SUB_STATE_S2_2_THINKING &&
                to_s2_sub_state == JULIA_S2_SUB_STATE_S2_3_SPEAKING) ||
               (from_s2_sub_state == JULIA_S2_SUB_STATE_S2_3_SPEAKING &&
                to_s2_sub_state == JULIA_S2_SUB_STATE_S2_1_LISTENING);

    case JULIA_MAIN_STATE_S3_STANDBY:
        return target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S4_INTERACTION) ||
               target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S6_SLEEP) ||
               target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S8_OTA);

    case JULIA_MAIN_STATE_S4_INTERACTION:
        return target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S3_STANDBY) ||
               target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S5_SILENT) ||
               target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S6_SLEEP) ||
               (to_main_state == JULIA_MAIN_STATE_S2_DIALOG &&
                to_s2_sub_state == JULIA_S2_SUB_STATE_S2_2_THINKING);

    case JULIA_MAIN_STATE_S5_SILENT:
        return target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S4_INTERACTION) ||
               target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S3_STANDBY);

    case JULIA_MAIN_STATE_S6_SLEEP:
        return target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S4_INTERACTION);

    case JULIA_MAIN_STATE_S8_OTA:
        return target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S0_BOOT) ||
               target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S3_STANDBY);
    case JULIA_MAIN_STATE_S7_FAULT:
    case JULIA_MAIN_STATE_COUNT:
    default:
        return false;
    }
}

bool julia_fsm_can_transition(julia_main_state_t from_main_state,
                              julia_s2_sub_state_t from_s2_sub_state,
                              julia_main_state_t to_main_state,
                              julia_s2_sub_state_t to_s2_sub_state)
{
    return julia_fsm_can_transition_full(from_main_state, from_s2_sub_state,
                                         from_main_state == JULIA_MAIN_STATE_S7_FAULT
                                             ? JULIA_S7_SUB_STATE_S7_2_FAULT
                                             : JULIA_S7_SUB_STATE_NONE,
                                         to_main_state, to_s2_sub_state,
                                         to_main_state == JULIA_MAIN_STATE_S7_FAULT
                                             ? JULIA_S7_SUB_STATE_S7_2_FAULT
                                             : JULIA_S7_SUB_STATE_NONE);
}

bool julia_fsm_transition_to_full(julia_fsm_t *fsm,
                                  julia_main_state_t to_main_state,
                                  julia_s2_sub_state_t to_s2_sub_state,
                                  julia_s7_sub_state_t to_s7_sub_state,
                                  fsm_event_t reason)
{
    if (fsm == NULL || reason >= EVT_COUNT ||
        !julia_fsm_can_transition_full(fsm->main_state, fsm->s2_sub_state,
                                       fsm->s7_sub_state, to_main_state,
                                       to_s2_sub_state, to_s7_sub_state)) return false;

    julia_main_state_t from_main_state = fsm->main_state;
    julia_s2_sub_state_t from_s2_sub_state = fsm->s2_sub_state;
    julia_s7_sub_state_t from_s7_sub_state = fsm->s7_sub_state;
    if (fsm->on_exit != NULL) {
        fsm->on_exit(fsm, from_main_state, from_s2_sub_state, reason);
    }
    fsm->main_state = to_main_state;
    fsm->s2_sub_state = to_s2_sub_state;
    fsm->s7_sub_state = to_s7_sub_state;
    ESP_LOGI(TAG, "[FSM] %s/%s/%s -> %s/%s/%s (%s)",
             julia_fsm_main_state_name(from_main_state),
             julia_fsm_s2_sub_state_name(from_s2_sub_state),
             julia_fsm_s7_sub_state_name(from_s7_sub_state),
             julia_fsm_main_state_name(to_main_state),
             julia_fsm_s2_sub_state_name(to_s2_sub_state),
             julia_fsm_s7_sub_state_name(to_s7_sub_state),
             julia_fsm_event_name(reason));
    if (fsm->on_enter != NULL) {
        fsm->on_enter(fsm, to_main_state, to_s2_sub_state, reason);
    }
    return true;
}

bool julia_fsm_transition_to(julia_fsm_t *fsm,
                             julia_main_state_t to_main_state,
                             julia_s2_sub_state_t to_s2_sub_state,
                             fsm_event_t reason)
{
    return julia_fsm_transition_to_full(fsm, to_main_state, to_s2_sub_state,
                                        to_main_state == JULIA_MAIN_STATE_S7_FAULT
                                            ? JULIA_S7_SUB_STATE_S7_2_FAULT
                                            : JULIA_S7_SUB_STATE_NONE,
                                        reason);
}

void julia_fsm_init(julia_fsm_t *fsm)
{
    if (fsm == NULL) return;
    fsm->main_state = JULIA_MAIN_STATE_S0_BOOT;
    fsm->s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    fsm->s7_sub_state = JULIA_S7_SUB_STATE_NONE;
    fsm->on_enter = default_on_enter;
    fsm->on_exit = default_on_exit;
    fsm->user_ctx = NULL;
    fsm->on_enter(fsm, fsm->main_state, fsm->s2_sub_state, EVT_NONE);
}

bool julia_fsm_handle_event(julia_fsm_t *fsm, fsm_event_t event, void *data)
{
    (void)data;
    if (fsm == NULL ||
        !julia_fsm_state_is_valid_full(fsm->main_state, fsm->s2_sub_state,
                                       fsm->s7_sub_state) ||
        event <= EVT_NONE || event >= EVT_COUNT) return false;

    julia_main_state_t target_main_state = JULIA_MAIN_STATE_COUNT;
    julia_s2_sub_state_t target_s2_sub_state = JULIA_S2_SUB_STATE_COUNT;
    julia_s7_sub_state_t target_s7_sub_state = JULIA_S7_SUB_STATE_NONE;

    if ((fsm->main_state == JULIA_MAIN_STATE_S1_COMPANION ||
         fsm->main_state == JULIA_MAIN_STATE_S2_DIALOG ||
         fsm->main_state == JULIA_MAIN_STATE_S4_INTERACTION) &&
        (event == EVT_MQTT_DISCONNECTED || event == EVT_WSS_DISCONNECTED)) {
        /* 控制消息或语音数据任一连接断开后，本轮交流都不再完整。设备先显示断联
         * 提示，再返回待机，等连接自动恢复后由下一次唤醒重新开始。 */
        target_main_state = JULIA_MAIN_STATE_S7_FAULT;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
        target_s7_sub_state = JULIA_S7_SUB_STATE_S7_1_DISCONNECTED;
    } else if (fsm->main_state == JULIA_MAIN_STATE_S7_FAULT &&
               fsm->s7_sub_state == JULIA_S7_SUB_STATE_S7_1_DISCONNECTED &&
               event == EVT_DISCONNECT_NOTICE_TIMEOUT) {
        /* 三秒足以让用户看到本轮交流为何结束；随后回到默认待机，避免断联画面
         * 长期占据屏幕。网络连接由各自后台任务继续恢复。 */
        target_main_state = JULIA_MAIN_STATE_S3_STANDBY;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if (fsm->main_state == JULIA_MAIN_STATE_S1_COMPANION &&
               event == EVT_USER_LEAVE) {
        /* 对话结束后的连续交流窗口已超时，重新要求唤醒词。 */
        target_main_state = JULIA_MAIN_STATE_S3_STANDBY;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if ((fsm->main_state == JULIA_MAIN_STATE_S0_BOOT ||
                fsm->main_state == JULIA_MAIN_STATE_S1_COMPANION ||
                fsm->main_state == JULIA_MAIN_STATE_S3_STANDBY) &&
               event == EVT_OTA_AVAILABLE) {
        target_main_state = JULIA_MAIN_STATE_S8_OTA;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if (fsm->main_state == JULIA_MAIN_STATE_S3_STANDBY &&
               event == EVT_WAKEUP) {
        /* 只有已经确认的唤醒词才能让待机设备开始一轮交流。 */
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
                fsm->main_state == JULIA_MAIN_STATE_S2_DIALOG) &&
               event == EVT_INTENT_GOODNIGHT) {
        /* “晚安”结束本轮沟通并立即进入睡眠，不再绕经 S5/S3 计时。 */
        target_main_state = JULIA_MAIN_STATE_S6_SLEEP;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if ((fsm->main_state == JULIA_MAIN_STATE_S4_INTERACTION ||
                fsm->main_state == JULIA_MAIN_STATE_S2_DIALOG) &&
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
        /* OTA 普通失败回到需要唤醒词的待机态，避免让 S1 承担待唤醒语义。 */
        target_main_state = JULIA_MAIN_STATE_S3_STANDBY;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    /* 用户说完后等待回答，收到回答后播放；播放完成进入免唤醒陪伴窗口。 */
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
        return julia_fsm_transition_to_full(fsm, target_main_state,
                                            target_s2_sub_state,
                                            target_s7_sub_state, event);
    }

    ESP_LOGI(TAG, "[FSM] 事件尚未映射：%s，当前状态=%s/%s/%s",
             julia_fsm_event_name(event),
             julia_fsm_main_state_name(fsm->main_state),
             julia_fsm_s2_sub_state_name(fsm->s2_sub_state),
             julia_fsm_s7_sub_state_name(fsm->s7_sub_state));
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

const char *julia_fsm_s7_sub_state_name(julia_s7_sub_state_t state)
{
    return state < JULIA_S7_SUB_STATE_COUNT && s_s7_sub_state_names[state] != NULL
               ? s_s7_sub_state_names[state] : "UNKNOWN_S7_SUB";
}

const char *julia_fsm_event_name(fsm_event_t event)
{
    return event >= EVT_NONE && event < EVT_COUNT && s_event_names[event] != NULL
               ? s_event_names[event] : "UNKNOWN_EVT";
}

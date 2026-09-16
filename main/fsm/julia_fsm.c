/**
 * @file julia_fsm.c
 * @brief 定义设备在开机、待机、对话、睡眠、故障和升级之间如何切换。
 *
 * 每个外部事件先按当前业务状态确定去向，再由统一入口执行变化。这样可以拒绝
 * 不符合产品流程的跳转，例如设备尚未被唤醒时直接进入播放回答。
 *
 * 状态表不变量：
 *   - S2 子状态只在 S2 下非 NONE，S7 子状态只在 S7 下非 NONE，其余主状态两者都必须为 NONE；
 *   - S7.1 是一次性断联提示：进入时记录 s7_return_state，退出只能回到该落点或升级为 S7.2；
 *   - S7 不是可驻留状态，S7.2 只能通过复位离开。
 *
 * 校验与迁移判定分开：julia_fsm_state_is_valid* 回答“这个状态组合是否自洽”，
 * julia_fsm_can_transition* 回答“产品流程是否允许这次变化”。
 */
#include "julia_fsm.h"

#include <stddef.h>

#include "esp_log.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

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
    [EVT_VOICE_BUSY] = "EVT_VOICE_BUSY",
    [EVT_VOICE_SESSION_RESET] = "EVT_VOICE_SESSION_RESET",
    [EVT_REQUIRE_WAKE] = "EVT_REQUIRE_WAKE",
    [EVT_NONE] = "EVT_NONE",
    [EVT_USER_LEAVE] = "EVT_USER_LEAVE",
    [EVT_USER_CALL] = "EVT_USER_CALL",
    [EVT_LOCAL_SPEECH_START] = "EVT_LOCAL_SPEECH_START",
    [EVT_SILENCE_TIMEOUT] = "EVT_SILENCE_TIMEOUT",
    [EVT_NIGHT_TIME] = "EVT_NIGHT_TIME",
    [EVT_STANDBY_TIMEOUT] = "EVT_STANDBY_TIMEOUT",
    [EVT_SILENT_TIMEOUT] = "EVT_SILENT_TIMEOUT",
    [EVT_BEDTIME] = "EVT_BEDTIME",
    [EVT_START_DIALOG] = "EVT_START_DIALOG",
    [EVT_MULTI_TURN_DETECTED] = "EVT_MULTI_TURN_DETECTED",
    [EVT_INTERRUPT] = "EVT_INTERRUPT",
    [EVT_WAKEUP] = "EVT_WAKEUP",
    [EVT_MOTION_WAKE] = "EVT_MOTION_WAKE",
    [EVT_IMU_UNAVAILABLE] = "EVT_IMU_UNAVAILABLE",
    [EVT_INTENT_GOODNIGHT] = "EVT_INTENT_GOODNIGHT",
    [EVT_INTENT_DISMISS] = "EVT_INTENT_DISMISS",
    [EVT_MQTT_DISCONNECTED] = "EVT_MQTT_DISCONNECTED",
    [EVT_WSS_DISCONNECTED] = "EVT_WSS_DISCONNECTED",
    [EVT_MQTT_CONNECTED] = "EVT_MQTT_CONNECTED",
    [EVT_WSS_CONNECTED] = "EVT_WSS_CONNECTED",
    [EVT_SERVICE_CONNECT_TIMEOUT] = "EVT_SERVICE_CONNECT_TIMEOUT",
    [EVT_DISCONNECT_NOTICE_TIMEOUT] = "EVT_DISCONNECT_NOTICE_TIMEOUT",
    [EVT_OTA_AVAILABLE] = "EVT_OTA_AVAILABLE",
    [EVT_OTA_SUCCEEDED] = "EVT_OTA_SUCCEEDED",
    [EVT_OTA_TASK_FAILED] = "EVT_OTA_TASK_FAILED",
    [EVT_PREPARE_TERMINAL_REPLY] = "EVT_PREPARE_TERMINAL_REPLY",
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

/**
 * 只区分“S7 即 S7.2”的两参数版本，供只关心主状态与 S2 子状态的调用方使用
 * （故障快照的参数校验）。它把任何 S7 都当作 S7.2，因此不能用来判断 S7.1 断联提示
 * 是否合法，那类判断必须使用 _full 版本。
 */
bool julia_fsm_state_is_valid(julia_main_state_t main_state,
                              julia_s2_sub_state_t s2_sub_state)
{
    return julia_fsm_state_is_valid_full(main_state, s2_sub_state,
        main_state == JULIA_MAIN_STATE_S7_FAULT
            ? JULIA_S7_SUB_STATE_S7_2_FAULT
            : JULIA_S7_SUB_STATE_NONE);
}

/**
 * 状态自洽性的唯一定义：越界枚举、S2 子状态离开 S2、S7 子状态离开 S7，以及 S2 同时带
 * S7 子状态都算非法。运行时的周期巡检和故障快照写入都依赖这套判定，新增状态组合前必须
 * 先在这里成立，否则会被判为损坏或直接拒收。
 */
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

/* 下面的判定都要求目标没有任何子状态，普通迁移不能顺手把 S2/S7 子状态带过去。 */
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

/**
 * 完整迁移判定：只回答“产品流程是否允许这次变化”，既不修改状态也不涉及呈现。
 * 与状态校验的分工是——校验回答“组合是否自洽”，这里回答“两个自洽的组合之间能否走”。
 *
 * S7 落点规则集中在这里：S7.2 只能以复位（回 S0）离开，S7.1 只能回到预先记录的稳定
 * 落点或升级为 S7.2；主动安静（S5/S6）不接受断联提示。
 */
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

    /* S7.2 严重故障只能通过复位重新开机；S7.1 是可恢复提示，显示结束后回到
     * 预先记录的稳定落点。提示期间发生核心故障时可升级为 S7.2。 */
    if (from_main_state == JULIA_MAIN_STATE_S7_FAULT) {
        if (from_s7_sub_state == JULIA_S7_SUB_STATE_S7_1_DISCONNECTED) {
            return target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                             JULIA_MAIN_STATE_S3_STANDBY) ||
                   target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                             JULIA_MAIN_STATE_S5_SILENT) ||
                   target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                             JULIA_MAIN_STATE_S6_SLEEP) ||
                   target_is_fault(to_main_state, to_s2_sub_state,
                                   to_s7_sub_state);
        }
        return target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S0_BOOT);
    }
    if (target_is_fault(to_main_state, to_s2_sub_state, to_s7_sub_state)) return true;
    if (target_is_disconnected(to_main_state, to_s2_sub_state, to_s7_sub_state)) {
        /* S1/S2/S4 都绑定当前 WSS generation；断联后免唤醒资格和旧会话不可恢复，
         * 因此落到 S3。S3/S5/S6 不依赖旧会话，可在提示后恢复原状态。 */
        return from_main_state == JULIA_MAIN_STATE_S1_COMPANION ||
               from_main_state == JULIA_MAIN_STATE_S2_DIALOG ||
               from_main_state == JULIA_MAIN_STATE_S3_STANDBY ||
               from_main_state == JULIA_MAIN_STATE_S4_INTERACTION;
    }

    switch (from_main_state) {
    case JULIA_MAIN_STATE_S0_BOOT:
        return target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S3_STANDBY);

    case JULIA_MAIN_STATE_S1_COMPANION:
        return target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S4_INTERACTION) ||
               (to_main_state == JULIA_MAIN_STATE_S2_DIALOG &&
                to_s2_sub_state == JULIA_S2_SUB_STATE_S2_1_LISTENING) ||
               target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S3_STANDBY) ||
               target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                          JULIA_MAIN_STATE_S6_SLEEP);

    case JULIA_MAIN_STATE_S2_DIALOG:
        if (from_s2_sub_state == JULIA_S2_SUB_STATE_S2_3_SPEAKING &&
            target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                      JULIA_MAIN_STATE_S1_COMPANION)) return true;
        /* 终止语义可能晚于 MIC_STOP 到达；先回 S4 播放回应，再完成退出。
         * 播放不可用时仍允许直接退出。语音服务会先停止尚未播完的声音。 */
        if (target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                      JULIA_MAIN_STATE_S4_INTERACTION) ||
            target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
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
                (to_s2_sub_state == JULIA_S2_SUB_STATE_S2_2_THINKING ||
                 to_s2_sub_state == JULIA_S2_SUB_STATE_S2_1_LISTENING));

    case JULIA_MAIN_STATE_S5_SILENT:
        return target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                          JULIA_MAIN_STATE_S3_STANDBY) ||
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
               target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                          JULIA_MAIN_STATE_S4_INTERACTION) ||
#endif
               target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                          JULIA_MAIN_STATE_S6_SLEEP);

    case JULIA_MAIN_STATE_S6_SLEEP:
        return
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
               target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S4_INTERACTION) ||
#endif
               target_is(to_main_state, to_s2_sub_state, to_s7_sub_state,
                         JULIA_MAIN_STATE_S3_STANDBY);

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

/**
 * 不区分 S7 子状态的兼容入口：查询时把 S7 一律视为 S7.2，因此它无法表达 S7.1 的历史
 * 落点规则。真正的落点校验在 julia_fsm_transition_to_full 内完成。
 */
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

/* 唯一允许改写状态字段的入口：先 on_exit 旧状态、再写字段、最后 on_enter 新状态，
 * 因此 observer 看到的永远是已经生效的状态。S7.1 的落点检查与 s7_return_state 记录
 * 同样在这里完成，调用方不得自行改写字段。 */
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
    if (from_main_state == JULIA_MAIN_STATE_S7_FAULT &&
        from_s7_sub_state == JULIA_S7_SUB_STATE_S7_1_DISCONNECTED &&
        to_main_state != JULIA_MAIN_STATE_S7_FAULT &&
        to_main_state != fsm->s7_return_state) {
        return false;
    }
    if (fsm->on_exit != NULL) {
        fsm->on_exit(fsm, from_main_state, from_s2_sub_state, reason);
    }
    fsm->main_state = to_main_state;
    fsm->s2_sub_state = to_s2_sub_state;
    fsm->s7_sub_state = to_s7_sub_state;
    if (to_main_state == JULIA_MAIN_STATE_S7_FAULT &&
        to_s7_sub_state == JULIA_S7_SUB_STATE_S7_1_DISCONNECTED) {
        fsm->s7_return_state =
            from_main_state == JULIA_MAIN_STATE_S3_STANDBY ||
            from_main_state == JULIA_MAIN_STATE_S5_SILENT ||
            from_main_state == JULIA_MAIN_STATE_S6_SLEEP
                ? from_main_state
                : JULIA_MAIN_STATE_S3_STANDBY;
    }
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
    fsm->s7_return_state = JULIA_MAIN_STATE_S3_STANDBY;
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
    /* 主动静默不因连接关闭/旧唤醒词而退出，也不能被远程检查触发升级。
     * EVT_WAKEUP 只在未启用 capture-v1 时被拒绝：capture-v1 下安静状态仍接受唤醒词。 */
    if (julia_fsm_is_quiet(fsm->main_state) &&
        (
#if !CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
         event == EVT_WAKEUP ||
#endif
         event == EVT_MQTT_DISCONNECTED ||
         event == EVT_WSS_DISCONNECTED || event == EVT_SERVICE_CONNECT_TIMEOUT ||
         event == EVT_OTA_AVAILABLE)) return false;
    julia_s2_sub_state_t target_s2_sub_state = JULIA_S2_SUB_STATE_COUNT;
    julia_s7_sub_state_t target_s7_sub_state = JULIA_S7_SUB_STATE_NONE;

    /* 播放中被本地语音打断与新一轮听音共用同一落点，避免播放阶段的旧子状态残留。 */
    if (event == EVT_LOCAL_SPEECH_START &&
        (fsm->main_state == JULIA_MAIN_STATE_S1_COMPANION ||
         fsm->main_state == JULIA_MAIN_STATE_S4_INTERACTION ||
         (fsm->main_state == JULIA_MAIN_STATE_S2_DIALOG &&
          fsm->s2_sub_state == JULIA_S2_SUB_STATE_S2_3_SPEAKING))) {
        target_main_state = JULIA_MAIN_STATE_S2_DIALOG;
        target_s2_sub_state = JULIA_S2_SUB_STATE_S2_1_LISTENING;
    /* 云端声明旧会话不可继续：S1 的免唤醒资格与 S2/S4 的交互上下文一并作废。 */
    } else if ((event == EVT_VOICE_SESSION_RESET || event == EVT_VOICE_BUSY) &&
        (fsm->main_state == JULIA_MAIN_STATE_S1_COMPANION ||
         fsm->main_state == JULIA_MAIN_STATE_S2_DIALOG ||
         fsm->main_state == JULIA_MAIN_STATE_S4_INTERACTION)) {
        target_main_state = JULIA_MAIN_STATE_S3_STANDBY;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if ((fsm->main_state == JULIA_MAIN_STATE_S1_COMPANION ||
         fsm->main_state == JULIA_MAIN_STATE_S2_DIALOG ||
         fsm->main_state == JULIA_MAIN_STATE_S3_STANDBY ||
         fsm->main_state == JULIA_MAIN_STATE_S4_INTERACTION ||
         fsm->main_state == JULIA_MAIN_STATE_S5_SILENT ||
         fsm->main_state == JULIA_MAIN_STATE_S6_SLEEP) &&
        (event == EVT_MQTT_DISCONNECTED || event == EVT_WSS_DISCONNECTED ||
         event == EVT_SERVICE_CONNECT_TIMEOUT)) {
        /* 控制消息或语音数据任一连接断开后先进入 S7.1。S3/S5/S6 记录原状态；
         * S1/S2/S4 的返回点固定为 S3，禁止界面恢复已经失效的会话语义。 */
        target_main_state = JULIA_MAIN_STATE_S7_FAULT;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
        target_s7_sub_state = JULIA_S7_SUB_STATE_S7_1_DISCONNECTED;
    } else if (fsm->main_state == JULIA_MAIN_STATE_S7_FAULT &&
               fsm->s7_sub_state == JULIA_S7_SUB_STATE_S7_1_DISCONNECTED &&
               event == EVT_DISCONNECT_NOTICE_TIMEOUT) {
        /* 服务可用性由运行时正交维护；结束提示不等于连接已经恢复。 */
        target_main_state = fsm->s7_return_state;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if (fsm->main_state == JULIA_MAIN_STATE_S1_COMPANION &&
               (event == EVT_USER_LEAVE || event == EVT_REQUIRE_WAKE)) {
        /* 对话结束后的连续交流窗口已超时，重新要求唤醒词。 */
        target_main_state = JULIA_MAIN_STATE_S3_STANDBY;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    /* 仅 S3 准入升级：S5/S6 静默期间由上面的静默拒绝分支拦下 EVT_OTA_AVAILABLE，
     * 远程检查不会在用户主动休眠时触发升级。 */
    } else if (fsm->main_state == JULIA_MAIN_STATE_S3_STANDBY &&
               event == EVT_OTA_AVAILABLE) {
        target_main_state = JULIA_MAIN_STATE_S8_OTA;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if (
#if CONFIG_JULIA_LOCAL_CAPTURE_ENABLE
               (((fsm->main_state == JULIA_MAIN_STATE_S3_STANDBY ||
                   julia_fsm_is_quiet(fsm->main_state)) && event == EVT_MOTION_WAKE) ||
                (julia_fsm_is_quiet(fsm->main_state) && event == EVT_WAKEUP)) ||
#endif
               ((fsm->main_state == JULIA_MAIN_STATE_S3_STANDBY ||
                fsm->main_state == JULIA_MAIN_STATE_S1_COMPANION) &&
               event == EVT_WAKEUP)) {
        /* 唤醒词，或 capture-v1 下 S3/S5/S6 的明显搬动，均进入 S4 发起交互；
         * 未启用 capture-v1 时搬动不走进本分支，只由下面的兼容分支恢复待机。 */
        target_main_state = JULIA_MAIN_STATE_S4_INTERACTION;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if ((fsm->main_state == JULIA_MAIN_STATE_S3_STANDBY &&
                event == EVT_STANDBY_TIMEOUT) ||
               ((fsm->main_state == JULIA_MAIN_STATE_S1_COMPANION ||
                 fsm->main_state == JULIA_MAIN_STATE_S3_STANDBY ||
                 fsm->main_state == JULIA_MAIN_STATE_S5_SILENT) && event == EVT_NIGHT_TIME)) {
        /* 夜间窗口或 S3 驻留超时都进入睡眠态。 */
        target_main_state = JULIA_MAIN_STATE_S6_SLEEP;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if (julia_fsm_is_quiet(fsm->main_state) &&
               (event == EVT_MOTION_WAKE || event == EVT_IMU_UNAVAILABLE)) {
        /* 仅兼容路径（未启用 capture-v1）到达此处：搬动只恢复可见待机，不等同于用户已经
         * 发起一轮语音交流；capture-v1 下安静状态的 EVT_MOTION_WAKE 已由上面的分支送往 S4，
         * 这里实际兜住的是 EVT_IMU_UNAVAILABLE。 */
        target_main_state = JULIA_MAIN_STATE_S3_STANDBY;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if (fsm->main_state == JULIA_MAIN_STATE_S4_INTERACTION &&
               event == EVT_START_DIALOG) {
        /* 正常话语结束直接进入“想”，不要求服务端额外返回 dialog 意图。 */
        target_main_state = JULIA_MAIN_STATE_S2_DIALOG;
        target_s2_sub_state = JULIA_S2_SUB_STATE_S2_2_THINKING;
    } else if (fsm->main_state == JULIA_MAIN_STATE_S2_DIALOG &&
               event == EVT_PREPARE_TERMINAL_REPLY) {
        target_main_state = JULIA_MAIN_STATE_S4_INTERACTION;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if ((fsm->main_state == JULIA_MAIN_STATE_S4_INTERACTION ||
                fsm->main_state == JULIA_MAIN_STATE_S2_DIALOG) &&
               event == EVT_INTENT_GOODNIGHT) {
        /* “晚安”结束本轮沟通并立即进入睡眠，不再绕经 S5/S3 计时。 */
        target_main_state = JULIA_MAIN_STATE_S6_SLEEP;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if ((fsm->main_state == JULIA_MAIN_STATE_S4_INTERACTION ||
                fsm->main_state == JULIA_MAIN_STATE_S2_DIALOG) &&
               event == EVT_INTENT_DISMISS) {
        /* 明确结束沟通进入静默，S5 的独立计时到期后进入 S6。 */
        target_main_state = JULIA_MAIN_STATE_S5_SILENT;
        target_s2_sub_state = JULIA_S2_SUB_STATE_NONE;
    } else if (fsm->main_state == JULIA_MAIN_STATE_S5_SILENT &&
               event == EVT_SILENT_TIMEOUT) {
        target_main_state = JULIA_MAIN_STATE_S6_SLEEP;
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

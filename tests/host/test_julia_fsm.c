#include <assert.h>
#include <string.h>

#include "julia_fsm.h"

int main(void)
{
    julia_fsm_t fsm;
    julia_fsm_init(&fsm);

    assert(JULIA_MAIN_STATE_COUNT == 9);
    assert(fsm.main_state == JULIA_MAIN_STATE_S0_BOOT);
    assert(fsm.s2_sub_state == JULIA_S2_SUB_STATE_NONE);

    assert(julia_fsm_state_is_valid(JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_1_LISTENING));
    assert(julia_fsm_state_is_valid(JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_2_THINKING));
    assert(julia_fsm_state_is_valid(JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_3_SPEAKING));
    assert(!julia_fsm_state_is_valid(JULIA_MAIN_STATE_S4_INTERACTION,
                                     JULIA_S2_SUB_STATE_S2_1_LISTENING));
    assert(julia_fsm_state_is_valid(JULIA_MAIN_STATE_S4_INTERACTION,
                                    JULIA_S2_SUB_STATE_NONE));
    assert(strcmp(julia_fsm_s2_sub_state_name(JULIA_S2_SUB_STATE_S2_1_LISTENING),
                  "S2.1_LISTENING") == 0);
    assert(strcmp(julia_fsm_event_name(EVT_WIFI_DISCONNECTED),
                  "EVT_WIFI_DISCONNECTED") == 0);

    /* 主状态之间的允许迁移关系。 */
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S0_BOOT, JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S1_COMPANION, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S0_BOOT, JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S8_OTA, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S1_COMPANION, JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_1_LISTENING));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S1_COMPANION, JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S3_STANDBY, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S1_COMPANION, JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S8_OTA, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_3_SPEAKING,
                                    JULIA_MAIN_STATE_S1_COMPANION, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_1_LISTENING,
                                    JULIA_MAIN_STATE_S3_STANDBY, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_2_THINKING,
                                    JULIA_MAIN_STATE_S3_STANDBY, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_3_SPEAKING,
                                    JULIA_MAIN_STATE_S3_STANDBY, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S3_STANDBY, JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S4_INTERACTION, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S3_STANDBY, JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S6_SLEEP, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S4_INTERACTION,
                                    JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S5_SILENT, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S4_INTERACTION,
                                    JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S6_SLEEP, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S4_INTERACTION,
                                    JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S3_STANDBY, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S4_INTERACTION,
                                    JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_2_THINKING));
    assert(!julia_fsm_can_transition(JULIA_MAIN_STATE_S4_INTERACTION,
                                     JULIA_S2_SUB_STATE_NONE,
                                     JULIA_MAIN_STATE_S2_DIALOG,
                                     JULIA_S2_SUB_STATE_S2_1_LISTENING));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S5_SILENT, JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S4_INTERACTION, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S5_SILENT, JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S3_STANDBY, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S6_SLEEP, JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S4_INTERACTION, JULIA_S2_SUB_STATE_NONE));

    /* S2 内部唯一允许的循环是 S2.1 -> S2.2 -> S2.3 -> S2.1。 */
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_1_LISTENING,
                                    JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_2_THINKING));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_2_THINKING,
                                    JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_3_SPEAKING));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_3_SPEAKING,
                                    JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_1_LISTENING));
    assert(!julia_fsm_can_transition(JULIA_MAIN_STATE_S2_DIALOG,
                                     JULIA_S2_SUB_STATE_S2_1_LISTENING,
                                     JULIA_MAIN_STATE_S2_DIALOG,
                                     JULIA_S2_SUB_STATE_S2_3_SPEAKING));
    assert(!julia_fsm_can_transition(JULIA_MAIN_STATE_S2_DIALOG,
                                     JULIA_S2_SUB_STATE_S2_1_LISTENING,
                                     JULIA_MAIN_STATE_S1_COMPANION,
                                     JULIA_S2_SUB_STATE_NONE));
    assert(!julia_fsm_can_transition(JULIA_MAIN_STATE_S2_DIALOG,
                                     JULIA_S2_SUB_STATE_S2_2_THINKING,
                                     JULIA_MAIN_STATE_S1_COMPANION,
                                     JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_1_LISTENING,
                                    JULIA_MAIN_STATE_S6_SLEEP,
                                    JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_2_THINKING,
                                    JULIA_MAIN_STATE_S5_SILENT,
                                    JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_3_SPEAKING,
                                    JULIA_MAIN_STATE_S6_SLEEP,
                                    JULIA_S2_SUB_STATE_NONE));

    /* 所有其他状态都能进入故障态；S7 只能返回 S0。 */
    for (julia_main_state_t state = JULIA_MAIN_STATE_S0_BOOT;
         state < JULIA_MAIN_STATE_COUNT; ++state) {
        if (state == JULIA_MAIN_STATE_S2_DIALOG || state == JULIA_MAIN_STATE_S7_FAULT)
            continue;
        assert(julia_fsm_can_transition(state, JULIA_S2_SUB_STATE_NONE,
                                        JULIA_MAIN_STATE_S7_FAULT,
                                        JULIA_S2_SUB_STATE_NONE));
    }
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S2_DIALOG,
                                    JULIA_S2_SUB_STATE_S2_2_THINKING,
                                    JULIA_MAIN_STATE_S7_FAULT,
                                    JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S7_FAULT,
                                    JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S0_BOOT,
                                    JULIA_S2_SUB_STATE_NONE));
    assert(!julia_fsm_can_transition(JULIA_MAIN_STATE_S7_FAULT,
                                     JULIA_S2_SUB_STATE_NONE,
                                     JULIA_MAIN_STATE_S1_COMPANION,
                                     JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S8_OTA,
                                    JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S0_BOOT,
                                    JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S8_OTA,
                                    JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S1_COMPANION,
                                    JULIA_S2_SUB_STATE_NONE));

    /* 受控迁移入口必须使用同一张允许迁移图。 */
    assert(!julia_fsm_transition_to(&fsm, JULIA_MAIN_STATE_S3_STANDBY,
                                    JULIA_S2_SUB_STATE_NONE, EVT_NONE));
    /* 开机初始化结果直接驱动 S0 -> S1，不额外制造事件。 */
    assert(julia_fsm_transition_to(&fsm, JULIA_MAIN_STATE_S1_COMPANION,
                                   JULIA_S2_SUB_STATE_NONE, EVT_NONE));
    assert(fsm.main_state == JULIA_MAIN_STATE_S1_COMPANION);

    /* S1 中的听、想、说直接复用项目现有语音事件。 */
    assert(julia_fsm_handle_event(&fsm, EVT_USER_CALL, NULL));
    assert(fsm.s2_sub_state == JULIA_S2_SUB_STATE_S2_1_LISTENING);
    assert(julia_fsm_handle_event(&fsm, EVT_START_DIALOG, NULL));
    assert(fsm.s2_sub_state == JULIA_S2_SUB_STATE_S2_2_THINKING);
    assert(julia_fsm_handle_event(&fsm, EVT_MULTI_TURN_DETECTED, NULL));
    assert(fsm.s2_sub_state == JULIA_S2_SUB_STATE_S2_3_SPEAKING);
    assert(julia_fsm_handle_event(&fsm, EVT_INTERRUPT, NULL));
    assert(fsm.s2_sub_state == JULIA_S2_SUB_STATE_S2_1_LISTENING);
    assert(julia_fsm_handle_event(&fsm, EVT_START_DIALOG, NULL));
    assert(julia_fsm_handle_event(&fsm, EVT_MULTI_TURN_DETECTED, NULL));
    assert(julia_fsm_handle_event(&fsm, EVT_SILENCE_TIMEOUT, NULL));
    assert(fsm.main_state == JULIA_MAIN_STATE_S1_COMPANION);
    assert(fsm.s2_sub_state == JULIA_S2_SUB_STATE_NONE);

    /* S3/S5/S6 只由唤醒词进入 S4；S4 等待尚未实现的服务端语义信号。 */
    julia_fsm_t wake_fsm;
    julia_fsm_init(&wake_fsm);
    assert(julia_fsm_transition_to(&wake_fsm, JULIA_MAIN_STATE_S1_COMPANION,
                                   JULIA_S2_SUB_STATE_NONE, EVT_NONE));
    assert(julia_fsm_handle_event(&wake_fsm, EVT_USER_LEAVE, NULL));
    assert(wake_fsm.main_state == JULIA_MAIN_STATE_S3_STANDBY);
    assert(julia_fsm_handle_event(&wake_fsm, EVT_NIGHT_TIME, NULL));
    assert(wake_fsm.main_state == JULIA_MAIN_STATE_S6_SLEEP);
    assert(!julia_fsm_handle_event(&wake_fsm, EVT_BEDTIME, NULL));
    assert(julia_fsm_handle_event(&wake_fsm, EVT_WAKEUP, NULL));
    assert(wake_fsm.main_state == JULIA_MAIN_STATE_S4_INTERACTION);
    assert(!julia_fsm_handle_event(&wake_fsm, EVT_USER_CALL, NULL));
    assert(wake_fsm.main_state == JULIA_MAIN_STATE_S4_INTERACTION);
    assert(julia_fsm_handle_event(&wake_fsm, EVT_START_DIALOG, NULL));
    assert(wake_fsm.main_state == JULIA_MAIN_STATE_S2_DIALOG);
    assert(wake_fsm.s2_sub_state == JULIA_S2_SUB_STATE_S2_2_THINKING);

    julia_fsm_t dismiss_fsm;
    julia_fsm_init(&dismiss_fsm);
    assert(julia_fsm_transition_to(&dismiss_fsm, JULIA_MAIN_STATE_S1_COMPANION,
                                   JULIA_S2_SUB_STATE_NONE, EVT_NONE));
    assert(julia_fsm_handle_event(&dismiss_fsm, EVT_USER_LEAVE, NULL));
    assert(julia_fsm_handle_event(&dismiss_fsm, EVT_WAKEUP, NULL));
    assert(julia_fsm_handle_event(&dismiss_fsm, EVT_INTENT_GOODNIGHT, NULL));
    assert(dismiss_fsm.main_state == JULIA_MAIN_STATE_S6_SLEEP);
    assert(julia_fsm_handle_event(&dismiss_fsm, EVT_WAKEUP, NULL));
    assert(dismiss_fsm.main_state == JULIA_MAIN_STATE_S4_INTERACTION);
    assert(julia_fsm_handle_event(&dismiss_fsm, EVT_INTENT_DISMISS, NULL));
    assert(dismiss_fsm.main_state == JULIA_MAIN_STATE_S5_SILENT);
    assert(julia_fsm_handle_event(&dismiss_fsm, EVT_SILENT_TIMEOUT, NULL));
    assert(dismiss_fsm.main_state == JULIA_MAIN_STATE_S3_STANDBY);

    /* 从 S1 发起的普通听音位于 S2.1，晚安也必须直接进入 S6。 */
    julia_fsm_t dialog_goodnight_fsm;
    julia_fsm_init(&dialog_goodnight_fsm);
    assert(julia_fsm_transition_to(&dialog_goodnight_fsm,
                                   JULIA_MAIN_STATE_S1_COMPANION,
                                   JULIA_S2_SUB_STATE_NONE, EVT_NONE));
    assert(julia_fsm_handle_event(&dialog_goodnight_fsm, EVT_USER_CALL, NULL));
    assert(julia_fsm_handle_event(&dialog_goodnight_fsm,
                                  EVT_INTENT_GOODNIGHT, NULL));
    assert(dialog_goodnight_fsm.main_state == JULIA_MAIN_STATE_S6_SLEEP);

    /* 跨链路迟到的终止语义可从 S2.3 收尾；语音服务会先取消残留播放。 */
    julia_fsm_t speaking_dismiss_fsm;
    julia_fsm_init(&speaking_dismiss_fsm);
    assert(julia_fsm_transition_to(&speaking_dismiss_fsm,
                                   JULIA_MAIN_STATE_S1_COMPANION,
                                   JULIA_S2_SUB_STATE_NONE, EVT_NONE));
    assert(julia_fsm_handle_event(&speaking_dismiss_fsm, EVT_USER_CALL, NULL));
    assert(julia_fsm_handle_event(&speaking_dismiss_fsm, EVT_START_DIALOG, NULL));
    assert(julia_fsm_handle_event(&speaking_dismiss_fsm,
                                  EVT_MULTI_TURN_DETECTED, NULL));
    assert(julia_fsm_handle_event(&speaking_dismiss_fsm,
                                  EVT_INTENT_DISMISS, NULL));
    assert(speaking_dismiss_fsm.main_state == JULIA_MAIN_STATE_S5_SILENT);

    /* Wi-Fi 断联使所有网络交互态统一回到 S3，并清除 S2 子状态。 */
    const julia_s2_sub_state_t disconnected_s2_states[] = {
        JULIA_S2_SUB_STATE_S2_1_LISTENING,
        JULIA_S2_SUB_STATE_S2_2_THINKING,
        JULIA_S2_SUB_STATE_S2_3_SPEAKING,
    };
    for (size_t i = 0; i < sizeof(disconnected_s2_states) /
                            sizeof(disconnected_s2_states[0]); ++i) {
        julia_fsm_t disconnected_fsm;
        julia_fsm_init(&disconnected_fsm);
        assert(julia_fsm_transition_to(&disconnected_fsm,
                                       JULIA_MAIN_STATE_S1_COMPANION,
                                       JULIA_S2_SUB_STATE_NONE, EVT_NONE));
        assert(julia_fsm_handle_event(&disconnected_fsm, EVT_USER_CALL, NULL));
        if (disconnected_s2_states[i] >= JULIA_S2_SUB_STATE_S2_2_THINKING) {
            assert(julia_fsm_handle_event(&disconnected_fsm,
                                          EVT_START_DIALOG, NULL));
        }
        if (disconnected_s2_states[i] >= JULIA_S2_SUB_STATE_S2_3_SPEAKING) {
            assert(julia_fsm_handle_event(&disconnected_fsm,
                                          EVT_MULTI_TURN_DETECTED, NULL));
        }
        assert(disconnected_fsm.s2_sub_state == disconnected_s2_states[i]);
        assert(julia_fsm_handle_event(&disconnected_fsm,
                                      EVT_WIFI_DISCONNECTED, NULL));
        assert(disconnected_fsm.main_state == JULIA_MAIN_STATE_S3_STANDBY);
        assert(disconnected_fsm.s2_sub_state == JULIA_S2_SUB_STATE_NONE);
    }

    julia_fsm_t disconnected_fsm;
    julia_fsm_init(&disconnected_fsm);
    assert(julia_fsm_transition_to(&disconnected_fsm,
                                   JULIA_MAIN_STATE_S1_COMPANION,
                                   JULIA_S2_SUB_STATE_NONE, EVT_NONE));
    assert(julia_fsm_handle_event(&disconnected_fsm,
                                  EVT_WIFI_DISCONNECTED, NULL));
    assert(disconnected_fsm.main_state == JULIA_MAIN_STATE_S3_STANDBY);

    julia_fsm_t disconnected_s4_fsm;
    julia_fsm_init(&disconnected_s4_fsm);
    assert(julia_fsm_transition_to(&disconnected_s4_fsm,
                                   JULIA_MAIN_STATE_S1_COMPANION,
                                   JULIA_S2_SUB_STATE_NONE, EVT_NONE));
    assert(julia_fsm_handle_event(&disconnected_s4_fsm, EVT_USER_LEAVE, NULL));
    assert(julia_fsm_handle_event(&disconnected_s4_fsm, EVT_WAKEUP, NULL));
    assert(julia_fsm_handle_event(&disconnected_s4_fsm,
                                  EVT_WIFI_DISCONNECTED, NULL));
    assert(disconnected_s4_fsm.main_state == JULIA_MAIN_STATE_S3_STANDBY);

    /* 纯 FSM 测试只验证 S7 迁移边；记录与复位由运行时故障通道负责。 */
    assert(julia_fsm_transition_to(&fsm, JULIA_MAIN_STATE_S7_FAULT,
                                   JULIA_S2_SUB_STATE_NONE, EVT_NONE));
    assert(fsm.main_state == JULIA_MAIN_STATE_S7_FAULT);
    assert(julia_fsm_transition_to(&fsm, JULIA_MAIN_STATE_S0_BOOT,
                                   JULIA_S2_SUB_STATE_NONE, EVT_NONE));

    /* S3 驻留计时器使用独立事件进入 S6，不复用夜间事件。 */
    julia_fsm_t standby_fsm;
    julia_fsm_init(&standby_fsm);
    assert(julia_fsm_transition_to(&standby_fsm, JULIA_MAIN_STATE_S1_COMPANION,
                                   JULIA_S2_SUB_STATE_NONE, EVT_NONE));
    assert(julia_fsm_handle_event(&standby_fsm, EVT_USER_LEAVE, NULL));
    assert(julia_fsm_handle_event(&standby_fsm, EVT_STANDBY_TIMEOUT, NULL));
    assert(standby_fsm.main_state == JULIA_MAIN_STATE_S6_SLEEP);

    /* OTA 接受、普通失败和成功复位分别对应 S8、S1、S0。 */
    julia_fsm_t ota_fsm;
    julia_fsm_init(&ota_fsm);
    assert(julia_fsm_handle_event(&ota_fsm, EVT_OTA_AVAILABLE, NULL));
    assert(ota_fsm.main_state == JULIA_MAIN_STATE_S8_OTA);
    assert(julia_fsm_handle_event(&ota_fsm, EVT_OTA_SUCCEEDED, NULL));
    assert(ota_fsm.main_state == JULIA_MAIN_STATE_S0_BOOT);
    assert(julia_fsm_transition_to(&ota_fsm, JULIA_MAIN_STATE_S1_COMPANION,
                                   JULIA_S2_SUB_STATE_NONE, EVT_NONE));
    assert(julia_fsm_handle_event(&ota_fsm, EVT_OTA_AVAILABLE, NULL));
    assert(julia_fsm_handle_event(&ota_fsm, EVT_OTA_TASK_FAILED, NULL));
    assert(ota_fsm.main_state == JULIA_MAIN_STATE_S1_COMPANION);

    /* 尚未配置目标的其他事件不能改变状态。 */
    assert(!julia_fsm_handle_event(&fsm, EVT_WAKEUP, NULL));
    assert(fsm.main_state == JULIA_MAIN_STATE_S0_BOOT);
    assert(fsm.s2_sub_state == JULIA_S2_SUB_STATE_NONE);
    return 0;
}

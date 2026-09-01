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
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S3_STANDBY, JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S4_INTERACTION, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S3_STANDBY, JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S6_SLEEP, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S4_INTERACTION,
                                    JULIA_S2_SUB_STATE_NONE,
                                    JULIA_MAIN_STATE_S5_SILENT, JULIA_S2_SUB_STATE_NONE));
    assert(julia_fsm_can_transition(JULIA_MAIN_STATE_S4_INTERACTION,
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
    assert(!julia_fsm_can_transition(JULIA_MAIN_STATE_S8_OTA,
                                     JULIA_S2_SUB_STATE_NONE,
                                     JULIA_MAIN_STATE_S0_BOOT,
                                     JULIA_S2_SUB_STATE_NONE));

    /* 受控迁移入口必须使用同一张允许迁移图。 */
    assert(!julia_fsm_transition_to(&fsm, JULIA_MAIN_STATE_S3_STANDBY,
                                    JULIA_S2_SUB_STATE_NONE, EVT_NONE));
    assert(julia_fsm_handle_event(&fsm, EVT_BOOT_COMPLETE, NULL));
    assert(fsm.main_state == JULIA_MAIN_STATE_S1_COMPANION);

    /* 听、想、说直接复用项目现有的语音事件。 */
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

    /* S4 复用听事件后进入 S2.1，但 S4 本身仍是独立主状态。 */
    assert(julia_fsm_transition_to(&fsm, JULIA_MAIN_STATE_S3_STANDBY,
                                   JULIA_S2_SUB_STATE_NONE, EVT_NONE));
    assert(julia_fsm_transition_to(&fsm, JULIA_MAIN_STATE_S4_INTERACTION,
                                   JULIA_S2_SUB_STATE_NONE, EVT_NONE));
    assert(julia_fsm_handle_event(&fsm, EVT_USER_CALL, NULL));
    assert(fsm.main_state == JULIA_MAIN_STATE_S2_DIALOG);
    assert(fsm.s2_sub_state == JULIA_S2_SUB_STATE_S2_1_LISTENING);

    assert(julia_fsm_handle_event(&fsm, EVT_SYSTEM_FAULT, NULL));
    assert(fsm.main_state == JULIA_MAIN_STATE_S7_FAULT);
    assert(!julia_fsm_handle_event(&fsm, EVT_SYSTEM_FAULT, NULL));
    assert(julia_fsm_transition_to(&fsm, JULIA_MAIN_STATE_S0_BOOT,
                                   JULIA_S2_SUB_STATE_NONE, EVT_NONE));

    /* 尚未配置目标的其他事件不能改变状态。 */
    assert(!julia_fsm_handle_event(&fsm, EVT_WAKEUP, NULL));
    assert(fsm.main_state == JULIA_MAIN_STATE_S0_BOOT);
    assert(fsm.s2_sub_state == JULIA_S2_SUB_STATE_NONE);
    return 0;
}

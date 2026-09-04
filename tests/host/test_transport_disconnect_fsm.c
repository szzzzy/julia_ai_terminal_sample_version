#include <assert.h>
#include <string.h>

#include "julia_fsm.h"

static void set_state(julia_fsm_t *fsm, julia_main_state_t main_state,
                      julia_s2_sub_state_t sub_state)
{
    julia_fsm_init(fsm);
    assert(julia_fsm_state_is_valid(main_state, sub_state));
    fsm->main_state = main_state;
    fsm->s2_sub_state = sub_state;
    fsm->s7_sub_state = main_state == JULIA_MAIN_STATE_S7_FAULT
                            ? JULIA_S7_SUB_STATE_S7_2_FAULT
                            : JULIA_S7_SUB_STATE_NONE;
}

static void verify_disconnect_event(fsm_event_t event)
{
    const julia_s2_sub_state_t s2_states[] = {
        JULIA_S2_SUB_STATE_S2_1_LISTENING,
        JULIA_S2_SUB_STATE_S2_2_THINKING,
        JULIA_S2_SUB_STATE_S2_3_SPEAKING,
    };
    for (size_t i = 0; i < sizeof(s2_states) / sizeof(s2_states[0]); ++i) {
        julia_fsm_t fsm;
        set_state(&fsm, JULIA_MAIN_STATE_S2_DIALOG, s2_states[i]);
        assert(julia_fsm_handle_event(&fsm, event, NULL));
        assert(fsm.main_state == JULIA_MAIN_STATE_S7_FAULT);
        assert(fsm.s2_sub_state == JULIA_S2_SUB_STATE_NONE);
        assert(fsm.s7_sub_state == JULIA_S7_SUB_STATE_S7_1_DISCONNECTED);
        assert(julia_fsm_handle_event(&fsm, EVT_DISCONNECT_NOTICE_TIMEOUT, NULL));
        assert(fsm.main_state == JULIA_MAIN_STATE_S3_STANDBY);
    }

    const julia_main_state_t stable_states[] = {
        JULIA_MAIN_STATE_S3_STANDBY,
        JULIA_MAIN_STATE_S5_SILENT,
        JULIA_MAIN_STATE_S6_SLEEP,
    };
    for (size_t i = 0; i < sizeof(stable_states) / sizeof(stable_states[0]); ++i) {
        julia_fsm_t fsm;
        set_state(&fsm, stable_states[i], JULIA_S2_SUB_STATE_NONE);
        assert(julia_fsm_handle_event(&fsm, event, NULL));
        assert(fsm.main_state == JULIA_MAIN_STATE_S7_FAULT);
        assert(fsm.s7_sub_state == JULIA_S7_SUB_STATE_S7_1_DISCONNECTED);
        assert(fsm.s7_return_state == stable_states[i]);
        assert(!julia_fsm_handle_event(
            &fsm, event == EVT_WSS_DISCONNECTED ? EVT_MQTT_DISCONNECTED
                                                : EVT_WSS_DISCONNECTED,
            NULL));
        assert(julia_fsm_handle_event(&fsm, EVT_DISCONNECT_NOTICE_TIMEOUT, NULL));
        assert(fsm.main_state == stable_states[i]);
    }

    const julia_main_state_t session_bound_states[] = {
        JULIA_MAIN_STATE_S1_COMPANION,
        JULIA_MAIN_STATE_S4_INTERACTION,
    };
    for (size_t i = 0; i < sizeof(session_bound_states) /
                            sizeof(session_bound_states[0]); ++i) {
        julia_fsm_t fsm;
        set_state(&fsm, session_bound_states[i], JULIA_S2_SUB_STATE_NONE);
        assert(julia_fsm_handle_event(&fsm, event, NULL));
        assert(fsm.s7_return_state == JULIA_MAIN_STATE_S3_STANDBY);
        assert(julia_fsm_handle_event(&fsm, EVT_DISCONNECT_NOTICE_TIMEOUT, NULL));
        assert(fsm.main_state == JULIA_MAIN_STATE_S3_STANDBY);
    }

    const julia_main_state_t unaffected_states[] = {
        JULIA_MAIN_STATE_S0_BOOT,
        JULIA_MAIN_STATE_S7_FAULT,
        JULIA_MAIN_STATE_S8_OTA,
    };
    for (size_t i = 0;
         i < sizeof(unaffected_states) / sizeof(unaffected_states[0]); ++i) {
        julia_fsm_t fsm;
        set_state(&fsm, unaffected_states[i], JULIA_S2_SUB_STATE_NONE);
        assert(!julia_fsm_handle_event(&fsm, event, NULL));
        assert(fsm.main_state == unaffected_states[i]);
        assert(fsm.s2_sub_state == JULIA_S2_SUB_STATE_NONE);
    }
}

int main(void)
{
    assert(strcmp(julia_fsm_event_name(EVT_MQTT_DISCONNECTED),
                  "EVT_MQTT_DISCONNECTED") == 0);
    assert(strcmp(julia_fsm_event_name(EVT_WSS_DISCONNECTED),
                  "EVT_WSS_DISCONNECTED") == 0);
    assert(strcmp(julia_fsm_event_name(EVT_MQTT_CONNECTED),
                  "EVT_MQTT_CONNECTED") == 0);
    assert(strcmp(julia_fsm_event_name(EVT_WSS_CONNECTED),
                  "EVT_WSS_CONNECTED") == 0);
    assert(strcmp(julia_fsm_event_name(EVT_SERVICE_CONNECT_TIMEOUT),
                  "EVT_SERVICE_CONNECT_TIMEOUT") == 0);
    assert(strcmp(julia_fsm_s7_sub_state_name(
                      JULIA_S7_SUB_STATE_S7_1_DISCONNECTED),
                  "S7.1_DISCONNECTED") == 0);
    assert(strcmp(julia_fsm_s7_sub_state_name(
                      JULIA_S7_SUB_STATE_S7_2_FAULT),
                  "S7.2_FAULT") == 0);

    verify_disconnect_event(EVT_MQTT_DISCONNECTED);
    verify_disconnect_event(EVT_WSS_DISCONNECTED);
    return 0;
}

#include <assert.h>
#include <stdio.h>
#include "julia_fsm.h"
int main(void) {
    julia_fsm_t f;
    julia_fsm_init(&f);
    assert(julia_fsm_transition_to(&f, JULIA_MAIN_STATE_S3_STANDBY, JULIA_S2_SUB_STATE_NONE, EVT_NONE));
    assert(julia_fsm_handle_event(&f, EVT_WAKEUP, NULL));
    assert(f.main_state == JULIA_MAIN_STATE_S4_INTERACTION);
    assert(!julia_fsm_handle_event(&f, EVT_USER_LEAVE, NULL));
    assert(f.main_state == JULIA_MAIN_STATE_S4_INTERACTION);
    puts("CONFIRMED: S4 + idle EVT_USER_LEAVE remains S4");
    assert(julia_fsm_handle_event(&f, EVT_START_DIALOG, NULL));
    assert(f.s2_sub_state == JULIA_S2_SUB_STATE_S2_2_THINKING);
    assert(!julia_fsm_handle_event(&f, EVT_USER_LEAVE, NULL));
    assert(!julia_fsm_handle_event(&f, EVT_SILENCE_TIMEOUT, NULL));
    assert(f.s2_sub_state == JULIA_S2_SUB_STATE_S2_2_THINKING);
    puts("CONFIRMED: S2.2 ignores idle and silence events");
    assert(julia_fsm_handle_event(&f, EVT_INTENT_DISMISS, NULL));
    puts("Terminal intent remains an escape; this is not a kernel deadlock.");
    return 0;
}

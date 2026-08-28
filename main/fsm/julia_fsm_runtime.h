/**
 * @file julia_fsm_runtime.h
 * @brief Owns the single application FSM instance and serializes events.
 */
#pragma once

#include "esp_err.h"
#include "julia_fsm.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create the sole FSM instance, apply its initial state, and start its event task. */
esp_err_t julia_fsm_runtime_init(void);

/**
 * Queue one event for the FSM task. This function never blocks the caller.
 * Events posted before initialization or while the queue is full are rejected.
 */
esp_err_t julia_fsm_runtime_post(fsm_event_t event);

/** Return the latest committed sub-state. */
julia_sub_state_t julia_fsm_runtime_get_state(void);

#ifdef __cplusplus
}
#endif

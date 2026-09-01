/**
 * @file julia_fsm_runtime.h
 * @brief 持有应用唯一的 FSM 实例，并串行处理事件。
 */
#pragma once

#include "esp_err.h"
#include "julia_fsm.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t julia_fsm_runtime_init(void);
esp_err_t julia_fsm_runtime_post(fsm_event_t event);
/** 返回当前新版本主状态。 */
julia_main_state_t julia_fsm_runtime_get_state(void);
/** 仅当主状态为 S2 时返回 S2.1/S2.2/S2.3，其他状态返回 NONE。 */
julia_s2_sub_state_t julia_fsm_runtime_get_s2_sub_state(void);

#ifdef __cplusplus
}
#endif

#pragma once
#include "julia_fsm.h"
#include "esp_err.h"
typedef enum { JULIA_SERVICE_CONNECTING, JULIA_SERVICE_ONLINE, JULIA_SERVICE_OFFLINE }
    julia_service_state_t;
julia_main_state_t julia_fsm_runtime_get_state(void);
julia_service_state_t julia_fsm_runtime_get_service_state(void);
esp_err_t julia_fsm_runtime_post(fsm_event_t event);

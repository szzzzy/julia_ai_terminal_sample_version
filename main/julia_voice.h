#pragma once
#include "esp_err.h"
#include "julia_fsm.h"
esp_err_t julia_voice_init(void);
bool julia_voice_handle_event(fsm_event_t event);
julia_sub_state_t julia_voice_get_state(void);
bool julia_voice_is_busy(void);
void julia_voice_interrupt(void);
int64_t julia_voice_last_audio_activity_ms(void);

typedef enum {
    JULIA_VOICE_INJECT_WAKE = 0,
    JULIA_VOICE_INJECT_ASR_DONE,
    JULIA_VOICE_INJECT_LLM_RESPONSE,
    JULIA_VOICE_INJECT_TTS_READY,
    JULIA_VOICE_INJECT_TTS_DONE,
} julia_voice_injected_event_t;

/* 仅供诊断/烤机使用；内部复用真实 FSM 和 DialogPhase 事件路径。 */
esp_err_t julia_voice_inject_event(julia_voice_injected_event_t event, const char *text);

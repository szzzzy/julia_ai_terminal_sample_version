#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LED_S0_OFF = 0,
    LED_S1_DIM_WARM,
    LED_S2_SOFT_WARM,
    LED_S3_ALERT,
    LED_S4_EMOTION,
    LED_S5_FADE_COLD,
    LED_STATE_COUNT,
} led_state_t;

void led_set_state(led_state_t state);
void led_transition_to(led_state_t target, uint16_t duration_ms);
void led_set_emotion_color(uint32_t rgb);
void breathing_led_update(uint32_t now_ms);
bool breathing_led_transition_active(void);
void breathing_led_set_display_sleep(bool sleeping, bool deep_sleep);

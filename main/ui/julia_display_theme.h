#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "julia_fsm.h"

typedef void (*julia_backlight_setter_t)(uint8_t percent);

esp_err_t julia_display_theme_init(julia_backlight_setter_t setter);
void julia_display_theme_on_state(julia_sub_state_t state);
void julia_display_theme_on_state_transition(julia_sub_state_t state, uint16_t duration_ms);
void julia_display_theme_on_interaction(void);
void reset_idle_timer(void);
void julia_display_theme_update(uint32_t now_ms);
void julia_display_theme_set_idle_timeout(uint32_t milliseconds);
bool julia_display_theme_rendering(void);
void display_breathing_start(void);
void display_breathing_stop(void);
bool display_breathing_active(void);
void display_fade_in(uint16_t duration_ms);

#ifdef DISPLAY_DEBUG
void display_debug_force_breathing(void);
void display_debug_stop_breathing(void);
#endif

esp_err_t julia_theme_next(void);
esp_err_t julia_theme_select(const char *name);
const char *julia_theme_current(void);
size_t julia_theme_count(void);

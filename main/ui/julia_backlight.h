#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t julia_backlight_init(void);
void julia_backlight_set(uint8_t percent);
esp_err_t julia_backlight_fade_to(uint8_t percent, uint32_t duration_ms);
esp_err_t julia_backlight_wait_fade(uint32_t timeout_ms);
esp_err_t julia_backlight_breathe_start(uint8_t min_percent, uint8_t max_percent, uint32_t period_ms);
esp_err_t julia_backlight_breathe_start_ex(uint8_t min_percent, uint8_t max_percent,
                                           uint32_t period_ms, uint16_t segments);
esp_err_t julia_backlight_set_gamma(bool enabled);
bool julia_backlight_gamma_enabled(void);
void julia_backlight_breathe_stop(void);
bool julia_backlight_breathing(void);
uint8_t julia_backlight_get_percent(void);
uint32_t julia_backlight_get_duty(void);
int julia_backlight_get_gpio_level(void);
void julia_backlight_force_off(void);

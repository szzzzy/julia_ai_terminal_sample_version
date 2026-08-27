#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifndef JULIA_LED_GPIO
#define JULIA_LED_GPIO 21
#endif

typedef enum {
    JULIA_EMOTION_HAPPY = 0,
    JULIA_EMOTION_SAD,
    JULIA_EMOTION_ANGRY,
    JULIA_EMOTION_CALM,
    JULIA_EMOTION_CARING,
    JULIA_EMOTION_WORRIED,
} emotion_t;

esp_err_t julia_led_init(void);
void julia_led_set_breathing(uint8_t brightness_min, uint8_t brightness_max,
                             uint16_t period_ms, uint32_t color);
void julia_led_set_solid(uint8_t brightness, uint32_t color);
void julia_led_set_off(void);
void julia_led_set_emotion(emotion_t emotion);
uint32_t julia_led_hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value);


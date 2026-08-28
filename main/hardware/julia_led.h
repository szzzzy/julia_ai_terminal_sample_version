/**
 * @file    julia_led.h
 * @brief   WS2812 可寻址 LED 的低层驱动接口（亮度/颜色/呼吸曲线）。
 *
 * 说明：本头是“原语层”接口，实现见 julia_led.c；上层状态机（breathing_led.c）
 * 通过本接口把“某一时刻该亮多少/什么颜色”交给底层点亮。LED 数据脚在 GPIO21
 * （可用 JULIA_LED_GPIO 覆盖），通过 RMT 外设发送时序。
 *
 * 使用约定：
 * - 先调用 julia_led_init() 一次（在工作任务上下文），之后才能调用 set_*。
 * - set_* 只是“请求”配置，真正输出由内部 led_task 按 80ms 周期完成。
 * - julia_led_set_emotion()/hsv_to_rgb() 为配色与情感映射工具，与输出无关。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifndef JULIA_LED_GPIO
#define JULIA_LED_GPIO 21
#endif

/** 情感枚举：与 julia_led_set_emotion() 的颜色表一一对应。 */
typedef enum {
    JULIA_EMOTION_HAPPY = 0,
    JULIA_EMOTION_SAD,
    JULIA_EMOTION_ANGRY,
    JULIA_EMOTION_CALM,
    JULIA_EMOTION_CARING,
    JULIA_EMOTION_WORRIED,
} emotion_t;

/**
 * @brief 初始化 LED（RMT 通道 + 编码器 + 后台刷新任务），只调用一次。
 * @return ESP_OK 成功；其他 esp_err_t 初始化失败。
 */
esp_err_t julia_led_init(void);

/**
 * @brief 请求呼吸灯效果。
 * @param[in] brightness_min 亮度下限（0~100）。
 * @param[in] brightness_max 亮度上限（0~100）。
 * @param[in] period_ms      呼吸周期（ms）。
 * @param[in] color          颜色 0xRRGGBB。
 */
void julia_led_set_breathing(uint8_t brightness_min, uint8_t brightness_max,
                             uint16_t period_ms, uint32_t color);
/**
 * @brief 请求固定亮度常量亮。
 * @param[in] brightness 亮度（0~100）。
 * @param[in] color      颜色 0xRRGGBB。
 */
void julia_led_set_solid(uint8_t brightness, uint32_t color);
/** 关闭 LED。 */
void julia_led_set_off(void);
/**
 * @brief 按情感枚举设置常亮配色。
 * @param[in] emotion 情感枚举，超界忽略。
 */
void julia_led_set_emotion(emotion_t emotion);
/**
 * @brief HSV → RGB。
 * @param[in] hue        色相 0~360。
 * @param[in] saturation 饱和度 0~100。
 * @param[in] value      明度 0~100。
 * @return 颜色 0xRRGGBB。
 */
uint32_t julia_led_hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value);


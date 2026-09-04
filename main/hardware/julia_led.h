/**
 * @file    julia_led.h
 * @brief   通过 RMT 驱动单颗 WS2812，并异步应用颜色／亮度请求。
 *
 * 所有 set_* 只更新共享配置，唯一 led_task 最迟在下一刷新周期发送。必须先成功调用
 * julia_led_init()，且不得重复初始化。头文件 fallback GPIO21 与本板 LCD CS 冲突；
 * 主工程已通过编译定义改为 GPIO4，任何复用该头的其它 target 也必须显式提供已确认引脚。
 */
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

/**
 * @brief 创建 RMT channel、编码器、PM lock、mutex 和唯一刷新任务。
 * @note  非幂等；失败可能已分配部分资源，当前实现没有反初始化接口。
 */
esp_err_t julia_led_init(void);

/**
 * @brief 请求呼吸灯效果，亮度范围会钳位并按需交换。
 * @param[in] brightness_min 亮度下限（0~100）。
 * @param[in] brightness_max 亮度上限（0~100）。
 * @param[in] period_ms      呼吸周期（ms）；0 不会退化为常亮，应改用 set_solid。
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
/** 请求在下一刷新周期关闭 LED。 */
void julia_led_set_off(void);
/**
 * @brief 按固定颜色表请求 70% 常亮；这会直接替换当前 LED 模式。
 * @param[in] emotion 情感枚举，超界忽略。
 */
void julia_led_set_emotion(emotion_t emotion);
/**
 * @brief 纯计算 HSV → RGB，不访问 LED 资源，可在初始化前调用。
 * @param[in] hue        色相 0~360。
 * @param[in] saturation 饱和度 0~100。
 * @param[in] value      明度 0~100。
 * @return 颜色 0xRRGGBB。
 */
uint32_t julia_led_hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value);


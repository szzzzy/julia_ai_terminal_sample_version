/**
 * @file    breathing_led.h
 * @brief   LED 状态机接口：LED 状态 → 亮度/颜色轮廓 + 平滑过渡。
 *
 * 实现见 breathing_led.c。这是“策略层”，最终调用 julia_led_set_*（“原语层”）
 * 把灯点亮。UI 状态机通过 led_transition_to()/led_set_state() 表达“灯当前该是
 * 什么状态”，本模块负责把它翻译成亮度/颜色，并在状态间做时间插值。
 *
 * 使用约定：
 * - 在 UI 状态变化时调用 led_transition_to()/led_set_state()；
 * - 需要在固定节拍（如每 ~80ms）周期调用 breathing_led_update(now_ms) 推进过渡；
 * - breathing_led_set_display_sleep() 用于屏幕睡眠时压暗/切冷色。
 * 各函数无副作用，仅影响本模块内部状态与最终 LED 输出。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** LED 状态枚举：每个状态对应一个可配置的轮廓。 */
typedef enum {
    LED_S0_OFF = 0,
    LED_S1_DIM_WARM,
    LED_S2_SOFT_WARM,
    LED_S3_ALERT,
    LED_S4_EMOTION,
    LED_S5_FADE_COLD,
    LED_STATE_COUNT,
} led_state_t;

/** 立即切换到指定状态（无动画）。 */
void led_set_state(led_state_t state);
/** 从当前状态平滑过渡到目标状态，时长 duration_ms。 */
void led_transition_to(led_state_t target, uint16_t duration_ms);
/** 设置情感态颜色（作用于 LED_S4_EMOTION）。 */
void led_set_emotion_color(uint32_t rgb);
/** 过渡节拍器：每个刷新周期调用一次，推进过渡并输出。@param now_ms 当前毫秒。 */
void breathing_led_update(uint32_t now_ms);
/** 是否处于过渡中。 */
bool breathing_led_transition_active(void);
/** 设置屏幕睡眠模式（压暗/切冷色），并按新模式重刷 LED。 */
void breathing_led_set_display_sleep(bool sleeping, bool deep_sleep);

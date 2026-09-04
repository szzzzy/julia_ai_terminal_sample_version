/**
 * @file    breathing_led.h
 * @brief   将 LED 业务状态映射为轮廓，并按外部单调时钟推进过渡。
 *
 * 状态修改与 update 应由同一控制上下文串行调用；update 会进一步调用底层 LED API，
 * 因而只能在任务上下文运行。当前应用没有提供 update 节拍，也没有初始化底层 LED，
 * 所以该策略模块尚不是当前用户可见能力。
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

/** 立即应用指定轮廓；越界状态被忽略。 */
void led_set_state(led_state_t state);
/** 记录过渡目标；必须继续调用 breathing_led_update() 才会产生后续输出。 */
void led_transition_to(led_state_t target, uint16_t duration_ms);
/** 设置以后进入 LED_S4_EMOTION 时使用的颜色，不刷新当前输出。 */
void led_set_emotion_color(uint32_t rgb);
/** 使用与 esp_timer 相同的单调毫秒基准推进过渡；只允许一个 tick owner 调用。 */
void breathing_led_update(uint32_t now_ms);
/** 返回过渡标志的瞬时快照，不是跨任务同步屏障。 */
bool breathing_led_transition_active(void);
/** 设置屏幕睡眠模式（压暗/切冷色），并按新模式重刷 LED。 */
void breathing_led_set_display_sleep(bool sleeping, bool deep_sleep);

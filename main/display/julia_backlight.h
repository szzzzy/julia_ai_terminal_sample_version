/**
 * @file    julia_backlight.h
 * @brief   LCD 背光接口（LEDC PWM + 呼吸/渐变）。
 *
 * 背光是否点亮由调用方决定：init 后默认关闭，app_main 在首帧完整渲染后才点亮。
 * 各入口内部会先停呼吸（breathe_stop）避免与渐变/手动亮度抢占通道；除
 * julia_backlight_wait_fade() 会按 timeout_ms 等待渐变完成外，其余接口都不等待渐变结束
 * （控制路径共用一把递归锁，极端情况下会在锁上短暂阻塞）。亮度百分比统一按
 * 1023 * percent / 100 换算成 10bit 占空比。
 * 详见 julia_backlight.c。
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t julia_backlight_init(void);
/* 立即设到目标亮度（无渐变），并停掉正在进行的呼吸。 */
void julia_backlight_set(uint8_t percent);
/* 硬件渐变到目标亮度（duration_ms 内），不等待完成；需要等待时再调 wait_fade()。 */
esp_err_t julia_backlight_fade_to(uint8_t percent, uint32_t duration_ms);
/* 只等待最近一次 fade 完成信号，不校验它属于哪一次渐变；超时返回 ESP_ERR_TIMEOUT。 */
esp_err_t julia_backlight_wait_fade(uint32_t timeout_ms);
/* 呼吸区间与周期：要求 min<max 且 max≤100，固定 120 段（_ex 版本可指定段数，
 * 段数会按 period_ms 与 tick 精度裁剪）。 */
esp_err_t julia_backlight_breathe_start(uint8_t min_percent, uint8_t max_percent, uint32_t period_ms);
esp_err_t julia_backlight_breathe_start_ex(uint8_t min_percent, uint8_t max_percent,
                                           uint32_t period_ms, uint16_t segments);
/* gamma 只作用于呼吸曲线，不改变 set()/fade_to() 的直接亮度。 */
esp_err_t julia_backlight_set_gamma(bool enabled);
bool julia_backlight_gamma_enabled(void);
void julia_backlight_breathe_stop(void);
bool julia_backlight_breathing(void);
uint8_t julia_backlight_get_percent(void);
/* 读 LEDC 当前占空比计数（0..1023）；判断亮度请用它而不是回读引脚电平。 */
uint32_t julia_backlight_get_duty(void);
int julia_backlight_get_gpio_level(void);
/* 强制熄灭：停呼吸、停 LEDC 输出并暂停定时器。
 * 没有配对的恢复接口：julia_backlight_set()/fade_to()/breathe_start*() 会隐式恢复 PWM。 */
void julia_backlight_force_off(void);

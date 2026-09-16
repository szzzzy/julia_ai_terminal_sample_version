/**
 * @file    avatar_micro_action.h
 * @brief   未参与当前构建的三层微动作参考接口。
 *
 * 当前 julia_avatar 路径没有调用这些函数；下述周期与线程模型不适用于现有运行时。
 *
 * 接口本身不创建 Task 或 timer；调用方提供单调毫秒时间并负责串行调度。当前实现
 * 以 40 ms 为最小更新间隔，过密调用会被合并。线程与 LVGL 约束见
 * avatar_micro_motion.h；当前运行时不要接入本参考接口。
 */
#pragma once

#include <stdint.h>

/** 推进一帧参考微动作；now_ms 使用与 esp_timer 相同的单调毫秒基准。 */
void update_avatar(uint32_t now_ms);
/** 重排参考调度，不直接访问 LVGL 对象。 */
void on_user_interaction(void);


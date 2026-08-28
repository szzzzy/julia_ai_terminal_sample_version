/**
 * @file    julia_time.h
 * @brief   设备壁钟管理：本地时区恢复 + RTC 备份 + SNTP 网络校时。
 *
 * 职责边界：
 *   - 负责给出"可信的当前时间"，并把可信时间写回 RTC/系统时钟。它不关心这个
 *     时间被谁用（julia_context 用它判断夜间/22 点，julia_routine 用它做日常统计）。
 *   - 与 julia_context 的"时间同步"是两条独立路径：本模块由 app_main 在初始化时调用
 *     julia_time_init()（用 RTC 恢复系统时间），并在 IP-ready 时通过 julia_time_ip_ready()
 *     启动 SNTP（成功后把时间写回 RTC）。两处都会写 PCF85063，见 julia_context.c 注释。
 *
 * 依赖：板级 RTC 接口（board_rtc_*）、SNTP、时区宏 CONFIG_JULIA_TIMEZONE。
 *
 * 使用：app_main 调用 julia_time_init()；网络生命周期调用 julia_time_ip_ready()；
 *       julia_context 等通过 julia_time_valid() 查询墙钟是否已可信。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize timezone and restore system wall time from the onboard RTC. */
esp_err_t julia_time_init(void);

/**
 * @brief IP-ready 回调：网络取得 IPv4 后启动 SNTP 校时。
 *        （可注册到 network_lifecycle；仅首次生效，重复调用为无操作。）
 */
esp_err_t julia_time_ip_ready(void *arg);

/** Return true when either RTC restore or SNTP has provided a valid wall clock. */
bool julia_time_valid(void);

#ifdef __cplusplus
}
#endif


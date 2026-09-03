/**
 * @file    julia_time.h
 * @brief   让设备在断网启动时仍有合理时间，并在联网后自动校准和写回 RTC。
 *
 * 开机先读取板载 RTC；联网后再从 SNTP 获取更准确时间并写回 RTC。明显早于产品
 * 使用年代的时间视为未设置，夜间策略不会依据无效时间误触发。
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

/** 设置本地时区，并尝试从板载 RTC 恢复系统时间。 */
esp_err_t julia_time_init(void);

/**
 * @brief 网络可用后启动 SNTP 校时；重复通知不会创建第二个同步服务。
 */
esp_err_t julia_time_ip_ready(void *arg);

/** 返回设备是否已经获得足够可信、可用于夜间判断的当前时间。 */
bool julia_time_valid(void);

#ifdef __cplusplus
}
#endif

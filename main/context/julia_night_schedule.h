/**
 * @file julia_night_schedule.h
 * @brief 按可信墙钟产生夜间睡眠事件：本地时间夜间窗口（当前配置 23:00–07:00）。
 *
 * 只读取时间并投递 FSM 事件，不直接操作显示、音频或网络；睡眠宽限和"不打断交流"
 * 的判定都在实现内部完成。CONFIG_JULIA_NIGHT_SLEEP_ENABLE 关闭时，init 返回
 * ESP_ERR_NOT_SUPPORTED，notify 退化为空操作。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动夜间调度任务；重复调用复用已有任务。 */
esp_err_t julia_night_schedule_init(void);
/** 请求立即重算当前小时与睡眠宽限；从任务上下文调用，不得在 ISR 中使用。 */
void julia_night_schedule_notify(void);

#ifdef __cplusplus
}
#endif

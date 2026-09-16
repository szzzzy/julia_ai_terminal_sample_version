/**
 * @file julia_quiet_power.h
 * @brief 按行为状态编排采音与网络暂停。
 *
 * 本模块不做状态判定：安静与 OTA 的判断属于 FSM，这里只把已提交状态翻译成
 * MIC 与网络的开关动作。
 */
#pragma once
#include "esp_err.h"

/** 启动编排 Task。重复调用返回 ESP_OK 且不会创建第二个任务。 */
esp_err_t julia_quiet_power_init(void);

/**
 * 由 FSM state observer（voice_service）在每次状态变化后调用，只置位通知唤醒编排 Task；
 * 任务自行重新读取状态，因此这里不传状态、也不判断是否需要改变采音。
 */
void julia_quiet_power_notify(void);

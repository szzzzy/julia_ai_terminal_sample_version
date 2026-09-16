/**
 * @file julia_motion.h
 * @brief 运动唤醒输入；capture-v1 在 S3/S5/S6 监测，确认后由 FSM 进入 S4。
 *
 * 门限与节拍全部来自 Kconfig：采样周期（ms）、连续确认帧数、相邻样本加速度变化之和
 * （mg）、角速度模长（dps）与冷却时间（ms）。这些取值在仓库内没有标定记录，需要上板确认。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

/** 采样已开启且最近读数正常，供休眠编排判断唤醒输入是否可用。 */
bool julia_motion_ready(void);

/** 初始化共享 QMI8658 并启动运动检测；显示与背光仍由 FSM 独占。
 * 关闭 CONFIG_JULIA_IMU_MOTION_ENABLE 时返回 ESP_ERR_NOT_SUPPORTED。
 */
esp_err_t julia_motion_init(void);
/** 请求立即重新评估状态；从任务上下文调用，不得在 ISR 中使用。 */
void julia_motion_notify(void);

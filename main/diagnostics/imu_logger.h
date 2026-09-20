#pragma once
#include "esp_err.h"
/**
 * @file imu_logger.h
 * @brief 独立 IMU 实验入口（不是产品固件路径）。
 *
 * 生效条件：仅当 `CONFIG_JULIA_IMU_LOGGER_ENABLE=y` 时才被编入（Kconfig 默认 n）。打开后
 * 应用只启动本实验，不再启动产品语音/显示/行为 FSM。
 *
 * @note 前置条件：必须在电源、NVS、netif 与事件循环初始化完成之后调用（见 app/boot_coordinator.c
 *       的独立分支）；内部会启动 HTTP 服务与唯一的工作任务，重复调用前需先确认其实现语义。
 * @return ESP_OK 实验已启动；其他 esp_err_t 表示 HTTP 服务或资源创建失败。
 */
esp_err_t imu_logger_start(void);

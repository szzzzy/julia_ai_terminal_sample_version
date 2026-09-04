/**
 * @file qmi8658_shared.h
 * @brief 读取 QMI8658 加速度和陀螺仪，供 S6 运动诊断使用。
 *
 * 本接口复用 TCA9554 创建的共享 I2C bus，不拥有也不销毁总线。读写为阻塞操作，
 * 且内部没有并发锁；当前约定由单一运动监测任务调用。
 */
#pragma once

#include "esp_err.h"

typedef struct {
    float ax_g;
    float ay_g;
    float az_g;
    float gx_dps;
    float gy_dps;
    float gz_dps;
} board_imu_sample_t;

/**
 * @brief 探测 SA0 两种地址并配置为 30 Hz、±4 g、每轴 ±64 dps。
 *
 * 重复调用复用已验证的设备句柄。返回成功只表示寄存器配置完成，不表示上层运动
 * 门限可达或已经完成实机标定。
 */
esp_err_t board_imu_init(void);

/**
 * @brief 阻塞读取一个寄存器快照并换算为 g 和 dps。
 *
 * 数据不含时间戳或新鲜度标志；调用者负责采样节拍和失败后的基线重建。
 */
esp_err_t board_imu_read(board_imu_sample_t *sample);

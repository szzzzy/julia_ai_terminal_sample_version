/**
 * @file qmi8658_shared.h
 * @brief 读取 QMI8658 加速度和陀螺仪，供运动唤醒输入使用（当前在 S3/S5/S6 监测）。
 *
 * 本接口复用 TCA9554 创建的共享 I2C bus，不拥有也不销毁总线。读写为阻塞操作，
 * 且内部没有并发锁；当前约定由单一运动监测任务调用。
 *
 * 量程固定为加速度 ±4 g、陀螺仪每轴 ±64 dps（30 Hz ODR），换算结果单位为 g 和 dps。
 * 三轴合成角速度的理论上界约 110.9 dps（64×√3），调用方设置门限时必须留在该范围内。
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    float ax_g;
    float ay_g;
    float az_g;
    float gx_dps;
    float gy_dps;
    float gz_dps;
} board_imu_sample_t;

/**
 * @brief 探测并配置为 30 Hz、±4 g、每轴 ±64 dps，采样保持关闭。
 *
 * 重复调用复用已验证的设备句柄。返回成功只表示寄存器配置完成，不表示上层运动
 * 门限可达或已经完成实机标定。
 */
esp_err_t board_imu_init(void);

/** 单一运动任务切换采样；关闭清除 CTRL7 的 aEN/gEN，不切断板级供电。
 * 开启后调用者必须等待新样本并重建运动基线。失败时可重试。
 * 该开关直接改变其它读数的前提，因此只允许运动任务调用，其他上下文不要触碰。
 */
esp_err_t board_imu_set_enabled(bool enabled);

/**
 * @brief 阻塞读取一个寄存器快照并换算为 g 和 dps。
 *
 * 数据不含时间戳或新鲜度标志；调用者负责采样节拍和失败后的基线重建。
 */
esp_err_t board_imu_read(board_imu_sample_t *sample);

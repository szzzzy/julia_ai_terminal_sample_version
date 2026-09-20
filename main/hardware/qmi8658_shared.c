/**
 * @file qmi8658_shared.c
 * @brief 在板载共享 I2C 上提供 QMI8658 运动诊断所需的最小读数接口。
 *
 * TCA9554 是 I2C bus owner，本模块只向现有总线添加设备。初始化依次探测两个可能的
 * SA0 地址，并在 WHO_AM_I 不匹配时移除临时设备句柄，避免留下半初始化对象。
 *
 * 配置固定为 30 Hz、加速度 ±16 g、陀螺仪每轴 ±1024 dps；它服务于运动唤醒输入，
 * 不是通用姿态解算驱动。初始化保持采样关闭，由运动任务在 S3/S5/S6 开启
 * （S3 是否监测取决于 CONFIG_JULIA_LOCAL_CAPTURE_ENABLE，见 julia_motion.c）。
 *
 * 量程必须与标定录制一致，不要单独收窄：CONFIG_JULIA_IMU_ACCEL_DELTA_MG 与
 * CONFIG_JULIA_IMU_GYRO_THRESHOLD_DPS 由 tools/imu_logger/tune_motion.py 在
 * imu_records/latest 的 11 段典型场景上回放得出，而那批录制用的正是 ±16 g／±1024 dps。
 * 若收窄到每轴 ±64 dps，三轴合成模长上界降到 64×√3≈110.9 dps，低于 400 dps 门限，
 * 角速度支路恒不成立；而 delta 是相邻两轮读数的差分，轮询快于 ODR 时会重复读到同一批
 * 样本使 delta 归零、consecutive 清零，于是两条支路同时失效（见 julia_motion.c）。
 * 量程也不是越大越好：±1024 dps 已是 QMI8658C Rev A 的支持上限。改用任何更小量程前，
 * 必须用同一批录制重跑标定。
 */
#include "qmi8658_shared.h"

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "tca9554.h"

#define QMI8658_ADDR_LOW       0x6b
#define QMI8658_ADDR_HIGH      0x6a
#define QMI8658_WHO_AM_I       0x00
#define QMI8658_CTRL1          0x02
#define QMI8658_CTRL2          0x03
#define QMI8658_CTRL3          0x04
#define QMI8658_CTRL6          0x07
#define QMI8658_CTRL7          0x08
#define QMI8658_AX_L           0x35
#define QMI8658_EXPECTED_ID    0x05
#define QMI8658_I2C_TIMEOUT_MS 100

/* 位值来自 QMI8658 CTRL2/CTRL3 编码：量程在 bits[6:4]，30 Hz ODR 为低四位 8。 */
#define QMI8658_ACC_16G_30HZ     0x38
#define QMI8658_GYR_1024DPS_30HZ 0x68
#define QMI8658_ENABLE_ACC_GYR 0x43
#define QMI8658_AUTO_INCREMENT 0x40

/* 换算系数必须与写入 CTRL2/CTRL3 的量程一致：满量程除以 16 位有符号满刻度，读数的
 * 单位分别是 g 和 dps。只改量程而漏改这里，会得到静默缩放错误的读数（门限全部失准）。 */
#define ACC_G_PER_LSB    (16.0f / 32768.0f)
#define GYRO_DPS_PER_LSB (1024.0f / 32768.0f)

static const char *TAG = "QMI8658";
static i2c_master_dev_handle_t s_dev;
static uint8_t s_address;

/* 启动阶段配置完成后，由运动任务独占设备的采样控制和读数；不提供并发保护。 */

static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t length)
{
    if (s_dev == NULL || data == NULL || length == 0U) return ESP_ERR_INVALID_ARG;
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, length,
                                       QMI8658_I2C_TIMEOUT_MS);
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(s_dev, data, sizeof(data), QMI8658_I2C_TIMEOUT_MS);
}

/* SA0 决定的两个可能地址按 0x6B → 0x6A 顺序探测：探测失败必须摘掉临时设备句柄，
 * 否则下一次初始化会以为设备已就绪而直接复用错误的地址。 */
static esp_err_t try_address(i2c_master_bus_handle_t bus, uint8_t address)
{
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &config, &s_dev), TAG,
                        "add IMU device failed");
    uint8_t id = 0;
    esp_err_t err = read_regs(QMI8658_WHO_AM_I, &id, 1);
    if (err != ESP_OK || id != QMI8658_EXPECTED_ID) {
        ESP_LOGW(TAG, "probe 0x%02x failed result=%s id=0x%02x", address,
                 esp_err_to_name(err), id);
        (void)i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
    }
    s_address = address;
    return ESP_OK;
}

esp_err_t board_imu_init(void)
{
    if (s_dev != NULL) return ESP_OK;
    ESP_RETURN_ON_ERROR(tca9554_init(), TAG, "shared I2C bus unavailable");
    i2c_master_bus_handle_t bus = tca9554_i2c_bus();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "shared I2C bus is null");

    esp_err_t err = try_address(bus, QMI8658_ADDR_LOW);
    if (err != ESP_OK) err = try_address(bus, QMI8658_ADDR_HIGH);
    ESP_RETURN_ON_ERROR(err, TAG, "QMI8658 not found");

    err = board_imu_set_enabled(false);
    if (err == ESP_OK) err = write_reg(QMI8658_CTRL1, QMI8658_AUTO_INCREMENT);
    if (err == ESP_OK) err = write_reg(QMI8658_CTRL2, QMI8658_ACC_16G_30HZ);
    if (err == ESP_OK) err = write_reg(QMI8658_CTRL3, QMI8658_GYR_1024DPS_30HZ);
    if (err == ESP_OK) err = write_reg(QMI8658_CTRL6, 0x00);
    if (err != ESP_OK) {
        /* 句柄存在不代表配置成功；下一次必须重新探测和配置。 */
        (void)i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err;
    }

    ESP_LOGI(TAG, "ready address=0x%02x odr=30Hz accel=+/-16g gyro=+/-1024dps sampling=off", s_address);
    return ESP_OK;
}

esp_err_t board_imu_set_enabled(bool enabled)
{
    /* QMI8658C CTRL7: aEN bit0, gEN bit1。保留现有开启配置，关闭两传感器时写 0。 */
    return write_reg(QMI8658_CTRL7, enabled ? QMI8658_ENABLE_ACC_GYR : 0x00);
}

static int16_t le_i16(const uint8_t *data)
{
    return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}

/* 从 AX_L 起一次读 12 字节，依赖 CTRL1 的地址自增；读到的只是寄存器快照，
 * 与 30 Hz ODR 不对齐，也不带新鲜度标志，采样节拍由调用者负责。 */
esp_err_t board_imu_read(board_imu_sample_t *sample)
{
    if (sample == NULL) return ESP_ERR_INVALID_ARG;
    uint8_t data[12];
    ESP_RETURN_ON_ERROR(read_regs(QMI8658_AX_L, data, sizeof(data)), TAG,
                        "read accel/gyro");
    sample->ax_g = le_i16(&data[0]) * ACC_G_PER_LSB;
    sample->ay_g = le_i16(&data[2]) * ACC_G_PER_LSB;
    sample->az_g = le_i16(&data[4]) * ACC_G_PER_LSB;
    sample->gx_dps = le_i16(&data[6]) * GYRO_DPS_PER_LSB;
    sample->gy_dps = le_i16(&data[8]) * GYRO_DPS_PER_LSB;
    sample->gz_dps = le_i16(&data[10]) * GYRO_DPS_PER_LSB;
    return ESP_OK;
}

#if CONFIG_JULIA_IMU_LOGGER_ENABLE
esp_err_t board_imu_logger_configure(void)
{
    ESP_RETURN_ON_ERROR(board_imu_set_enabled(false), TAG, "disable for logger");
    ESP_RETURN_ON_ERROR(write_reg(QMI8658_CTRL2, 0x36), TAG, "logger accel 16g ODR6");
    ESP_RETURN_ON_ERROR(write_reg(QMI8658_CTRL3, 0x66), TAG, "logger gyro 1024dps ODR6");
    ESP_RETURN_ON_ERROR(write_reg(0x06, 0), TAG, "logger LPF disabled");
    uint8_t settings[4];
    ESP_RETURN_ON_ERROR(read_regs(QMI8658_CTRL2, settings, sizeof(settings)), TAG, "readback");
    if (settings[0] != 0x36 || settings[1] != 0x66 || settings[3] != 0)
        return ESP_ERR_INVALID_RESPONSE;
    /* CTRL7 bit6 在 Rev A 中为保留位，因此无需复制旧版的 wake 设置。 */
    return write_reg(QMI8658_CTRL7, 0x03);
}

esp_err_t board_imu_logger_read(uint32_t *counter, int16_t raw[6])
{
    if (!counter || !raw) return ESP_ERR_INVALID_ARG;
    uint8_t before[3], block[17];
    /* 先读一次 timestamp，再一次性突发读取 timestamp/temp/六个轴；
     * 最后比对 timestamp，丢弃任何跨越这几次传输才更新完成的采样。 */
    esp_err_t err = read_regs(0x30, before, sizeof(before));
    if (err != ESP_OK) return err;
    err = read_regs(0x30, block, sizeof(block));
    if (err != ESP_OK) return err;
    uint8_t after[3];
    err = read_regs(0x30, after, sizeof(after));
    if (err != ESP_OK) return err;
    for (unsigned i = 0; i < 3; ++i)
        if (before[i] != block[i] || after[i] != block[i]) return ESP_ERR_NOT_FINISHED;
    *counter = (uint32_t)block[0] | ((uint32_t)block[1] << 8) | ((uint32_t)block[2] << 16);
    for (unsigned i = 0; i < 6; ++i) raw[i] = le_i16(block + 5 + i * 2);
    return ESP_OK;
}
#endif

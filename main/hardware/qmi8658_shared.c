/**
 * @file qmi8658_shared.c
 * @brief Native ESP-IDF QMI8658 driver sharing the RTC/TCA9554 I2C bus.
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

/* CTRL2/3: scale in bits 4..6, ODR=30 Hz in low nibble (value 8). */
#define QMI8658_ACC_4G_30HZ    0x18
#define QMI8658_GYR_64DPS_30HZ 0x28
#define QMI8658_ENABLE_ACC_GYR 0x43
#define QMI8658_AUTO_INCREMENT 0x40

#define ACC_G_PER_LSB    (4.0f / 32768.0f)
#define GYRO_DPS_PER_LSB (64.0f / 32768.0f)

static const char *TAG = "QMI8658";
static i2c_master_dev_handle_t s_dev;
static uint8_t s_address;

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

    ESP_RETURN_ON_ERROR(write_reg(QMI8658_CTRL1, QMI8658_AUTO_INCREMENT), TAG,
                        "configure CTRL1");
    ESP_RETURN_ON_ERROR(write_reg(QMI8658_CTRL2, QMI8658_ACC_4G_30HZ), TAG,
                        "configure accelerometer");
    ESP_RETURN_ON_ERROR(write_reg(QMI8658_CTRL3, QMI8658_GYR_64DPS_30HZ), TAG,
                        "configure gyroscope");
    ESP_RETURN_ON_ERROR(write_reg(QMI8658_CTRL6, 0x00), TAG,
                        "disable attitude engine");
    ESP_RETURN_ON_ERROR(write_reg(QMI8658_CTRL7, QMI8658_ENABLE_ACC_GYR), TAG,
                        "enable accel/gyro");

    ESP_LOGI(TAG, "ready address=0x%02x odr=30Hz accel=+/-4g gyro=+/-64dps", s_address);
    return ESP_OK;
}

static int16_t le_i16(const uint8_t *data)
{
    return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}

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

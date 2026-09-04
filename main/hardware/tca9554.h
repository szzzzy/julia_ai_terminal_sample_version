/**
 * @file    tca9554.h
 * @brief   通过板载扩展器控制 LCD 复位和 SD 卡模式，并提供其它板载设备共用的 I2C 总线。
 *
 * SD 卡的模式选择信号不直接连接主芯片。挂载前必须通过扩展器保持高电平，否则
 * SD 卡可能进入 SPI 模式而无法按当前 SDMMC 接线工作。LCD 复位也经过同一扩展器。
 *
 * 本模块是板载 I2C bus owner；RTC/IMU 只能借用返回的 bus handle，不得删除总线或
 * 绕过本模块并行安装另一套 I2C driver。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

/** 7-bit I2C slave address；不要把 R/W 位拼入该值。 */
#define TCA9554_ADDR 0x20

/** P2 = Extend_IO3：SD 卡 D3/CS。SDMMC(SD) 模式下必须保持为高。 */
#define TCA9554_PIN_SD_CS 2

/** P1 = board schematic EXIO2: active-low ST77916 reset. */
#define TCA9554_PIN_LCD_RST 1

/**
 * @brief 初始化 I2C 主总线并添加 TCA9554 设备（幂等）。
 *
 * @note  创建 bus/device/mutex，只能在任务上下文调用；当前没有反初始化接口。
 */
esp_err_t tca9554_init(void);

/**
 * @brief 把引脚配置为推挽输出并设置电平。
 *
 * 先写输出锁存、再切方向，保证使能瞬间引脚不会出现意外电平。
 *
 * @param[in] pin   引脚号 0～7。
 * @param[in] level true 高电平；false 低电平。
 * @note  阻塞并持有模块 mutex；不得从 ISR 调用。
 */
esp_err_t tca9554_write_pin(uint8_t pin, bool level);

/**
 * @brief 读取引脚电平（输入或输出均可读）。
 *
 * @param[in]  pin   引脚号 0～7。
 * @param[out] level 输出当前电平。
 * @note  只在 I2C 读取成功时写 level；阻塞且不得从 ISR 调用。
 */
esp_err_t tca9554_read_pin(uint8_t pin, bool *level);

/** 返回借用的共享 bus handle；所有权仍属本模块，调用者不得删除。 */
i2c_master_bus_handle_t tca9554_i2c_bus(void);

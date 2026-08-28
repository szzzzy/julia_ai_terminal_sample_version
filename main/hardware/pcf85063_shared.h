/**
 * @file    pcf85063_shared.h
 * @brief   PCF85063 RTC 的“共享”接口（ESP-IDF i2c_master 封装），供时间服务使用。
 *
 * 实现见 pcf85063_shared.c。RTC（地址 0x51）位于由 tca9554 创建的共享 I2C 总线上，
 * 因此调用 board_rtc_init() 前无需单独建总线（它会内部自举 tca9554_init()）。
 *
 * 使用约定：
 * - 先 board_rtc_init()，再 board_rtc_ready() 判断是否可用；
 * - read_time/set_time 均为阻塞式 I2C 访问，需在任务上下文调用；
 * - 时间是 BCD→十进制后的标准字段：year 为公历（如 2024），dotw 0=周日…6=周六。
 * 本封装与 main/PCF85063/PCF85063.c 是两套并存实现，均操作同一颗 0x51 芯片。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** RTC 时间结构：字段为十进制（非 BCD），year 为完整公历年份。 */
typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t dotw;   /* 星期：0=周日 … 6=周六。 */
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} board_rtc_datetime_t;

/** 初始化共享 RTC（幂等）。@return ESP_OK 就绪；其他 esp_err_t 失败。 */
esp_err_t board_rtc_init(void);
/** RTC 是否已初始化可用。 */
bool board_rtc_ready(void);
/** 读取 RTC 时间到 time。@return ESP_OK 成功；其他 esp_err_t 失败。 */
esp_err_t board_rtc_read_time(board_rtc_datetime_t *time);
/** 写 RTC 时间。@param[in] time 目标时间（year 需在 1970~2069）。@return ESP_OK 成功。 */
esp_err_t board_rtc_set_time(const board_rtc_datetime_t *time);



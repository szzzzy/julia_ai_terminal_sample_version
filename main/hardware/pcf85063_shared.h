/**
 * @file    pcf85063_shared.h
 * @brief   读写板载 RTC，使设备断网重启后仍能恢复上次校准的本地时间。
 *
 * RTC 与扩展器共用板载 I2C，本模块会先确保总线存在。读取和写入都可能等待硬件，
 * 只能在普通任务中调用。
 *
 * 对外时间全部使用普通十进制字段，不暴露芯片内部 BCD 格式。年份范围受芯片两位
 * 年份限制；写入超出范围的时间会被拒绝。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** 用户可理解的本地日期时间；年份为完整公历年份，星期日为 0。 */
typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t dotw;   /* 星期：0=周日 … 6=周六。 */
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} board_rtc_datetime_t;

/** 初始化 RTC；重复调用复用同一设备。 */
esp_err_t board_rtc_init(void);
/** 查询 RTC 是否已经可以进行时间读写。 */
bool board_rtc_ready(void);
/** 读取断电保持的本地日期时间。 */
esp_err_t board_rtc_read_time(board_rtc_datetime_t *time);
/** 保存已校准的本地日期时间；年份必须在 1970～2069。 */
esp_err_t board_rtc_set_time(const board_rtc_datetime_t *time);


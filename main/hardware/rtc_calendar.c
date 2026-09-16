/**
 * @file rtc_calendar.c
 * @brief 在十进制日期时间与 PCF85063 的 7 字节 BCD 寄存器块之间转换。
 *
 * 寄存器块自秒起连续排列：秒、分、时、日、星期、月、年。除星期外每个字节都是两位
 * BCD（高半字节为十位）；年份只存两位十进制，真实年份 = 寄存器值 + year_base。
 * 星期按十进制 0～6 解释，0=周日。
 *
 * 当前构建使用本文件这一套实现；main/PCF85063/ 下的同名接口不参与构建，不能作为
 * 行为依据。
 */

#include "pcf85063_shared.h"

#include <stddef.h>

/* 日上限按月份表判断，闰年只按 year%4 处理：年份被限制在 1970～2069，该区间内能被 100
 * 整除的年份只有 2000，而 2000 能被 400 整除、本身就是闰年，因此不存在"整百年却按
 * year%4 误判"的情形，简化算法与公历一致。这里不校验 dotw 与年月日是否自洽。 */
bool board_rtc_datetime_valid(const board_rtc_datetime_t *time)
{
    if (time == NULL || time->year < 1970 || time->year > 2069 ||
        time->month == 0 || time->month > 12 || time->day == 0 || time->dotw > 6 ||
        time->hour > 23 || time->minute > 59 || time->second > 59) return false;
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned maximum = days[time->month - 1U];
    if (time->month == 2 && time->year % 4 == 0) ++maximum;
    return time->day <= maximum;
}

static uint8_t bcd(unsigned value) { return (uint8_t)((value / 10U) * 16U + value % 10U); }

/* 年份只保留两位（相对 1970 的偏移），因此范围外或非法日期一律返回 false，
 * 且不修改输出缓冲区，调用方可以安全地把失败当作“未写入”。 */
bool board_rtc_encode(const board_rtc_datetime_t *time, uint8_t data[7])
{
    if (data == NULL || !board_rtc_datetime_valid(time)) return false;
    data[0] = bcd(time->second); data[1] = bcd(time->minute); data[2] = bcd(time->hour);
    data[3] = bcd(time->day); data[4] = time->dotw; data[5] = bcd(time->month);
    data[6] = bcd(time->year - 1970U);
    return true;
}

/* 掩码只保留数据位：秒的 OS 停振标志、小时的 12/24 制标志和星期的保留位都不参与
 * 换算。任一 BCD 半字节大于 9、日期不成立或 year_base 不是 1970/2000 时返回 false，
 * 并保持 *time 不变——调用方据此区分“读到合法时间”与“RTC 内容不可用”。 */
bool board_rtc_decode(const uint8_t data[7], uint16_t year_base, board_rtc_datetime_t *time)
{
    if (data == NULL || time == NULL || (data[0] & 0x80U) ||
        (year_base != 1970 && year_base != 2000)) return false;
    const uint8_t masks[] = {0x7f,0x7f,0x3f,0x3f,0x07,0x1f,0xff};
    uint8_t value[7];
    for (unsigned i = 0; i < 7; ++i) {
        uint8_t raw = data[i] & masks[i];
        if ((raw & 15U) > 9U || (raw >> 4) > 9U) return false;
        value[i] = (uint8_t)((raw >> 4) * 10U + (raw & 15U));
    }
    board_rtc_datetime_t decoded = {.year = year_base + value[6], .month = value[5],
        .day = value[3], .dotw = value[4], .hour = value[2], .minute = value[1], .second = value[0]};
    if (!board_rtc_datetime_valid(&decoded)) return false;
    *time = decoded;
    return true;
}

#include "pcf85063_shared.h"

#include <stddef.h>

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

bool board_rtc_encode(const board_rtc_datetime_t *time, uint8_t data[7])
{
    if (data == NULL || !board_rtc_datetime_valid(time)) return false;
    data[0] = bcd(time->second); data[1] = bcd(time->minute); data[2] = bcd(time->hour);
    data[3] = bcd(time->day); data[4] = time->dotw; data[5] = bcd(time->month);
    data[6] = bcd(time->year - 1970U);
    return true;
}

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

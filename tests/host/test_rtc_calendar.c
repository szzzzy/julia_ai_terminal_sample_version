#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "pcf85063_shared.h"

int main(void)
{
    board_rtc_datetime_t time = {.year=2026,.month=2,.day=28,.dotw=6,.hour=23,.minute=59,.second=59};
    uint8_t bytes[7];
    assert(board_rtc_encode(&time, bytes) && bytes[6] == 0x56);
    board_rtc_datetime_t decoded;
    assert(board_rtc_decode(bytes, 1970, &decoded) && decoded.year == 2026);
    bytes[6] = 0x56;
    assert(board_rtc_decode(bytes, 1970, &decoded) && decoded.year == 2026);
    bytes[0] |= 0x80;
    assert(!board_rtc_decode(bytes, 1970, &decoded));
    bytes[0] = 0x6a;
    assert(!board_rtc_decode(bytes, 1970, &decoded));
    time.day = 29;
    assert(!board_rtc_encode(&time, bytes));
    time.year = 2024;
    assert(board_rtc_encode(&time, bytes) && bytes[6] == 0x54);
    time.year = 2028;
    assert(board_rtc_datetime_valid(&time));
    time.month = 4; time.day = 31;
    assert(!board_rtc_datetime_valid(&time));
    puts("PASS: RTC calendar, legacy year decoding, stop flag and leap days");
    return 0;
}

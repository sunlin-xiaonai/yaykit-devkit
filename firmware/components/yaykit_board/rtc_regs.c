#include "rtc_regs.h"

#define VALID_AFTER_S 1767225600LL /* 2026-01-01, as net_time */

static int from_bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static bool bcd_ok(uint8_t v) { return (v & 0x0F) <= 9 && (v >> 4) <= 9; }

/* days since 1970-01-01 (Howard Hinnant's days_from_civil) */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

bool rtc_regs_to_unix(const uint8_t r[RTC_TIME_REGS], int64_t *unix_s)
{
    if (r[0] & RTC_OS) return false;
    const uint8_t sec = r[0] & 0x7F, min = r[1] & 0x7F, hour = r[2] & 0x3F, day = r[3] & 0x3F, mon = r[5] & 0x1F, year = r[6];
    if (!bcd_ok(sec) || !bcd_ok(min) || !bcd_ok(hour) || !bcd_ok(day) || !bcd_ok(mon) || !bcd_ok(year)) return false;
    const int s = from_bcd(sec), mi = from_bcd(min), h = from_bcd(hour), d = from_bcd(day), mo = from_bcd(mon);
    if (s > 59 || mi > 59 || h > 23 || d < 1 || d > 31 || mo < 1 || mo > 12) return false;
    const int64_t t = days_from_civil(2000 + from_bcd(year), mo, d) * 86400 + h * 3600 + mi * 60 + s;
    if (t < VALID_AFTER_S) return false;
    *unix_s = t;
    return true;
}

void rtc_regs_from_unix(int64_t unix_s, uint8_t r[RTC_TIME_REGS])
{
    int64_t days = unix_s / 86400;
    const int64_t rem = unix_s % 86400;
    const int weekday = (int)((days + 4) % 7); /* 1970-01-01 was a Thursday; 0 = Sunday */
    days += 719468;
    const int64_t era = days / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp  = (5 * doy + 2) / 153;
    const int     d   = (int)(doy - (153 * mp + 2) / 5 + 1);
    const int     m   = (int)(mp < 10 ? mp + 3 : mp - 9);
    const int64_t y   = yoe + era * 400 + (m <= 2);
    r[0] = to_bcd((int)(rem % 60)); /* OS flag cleared: the time is good from now on */
    r[1] = to_bcd((int)(rem / 60 % 60));
    r[2] = to_bcd((int)(rem / 3600));
    r[3] = to_bcd(d);
    r[4] = (uint8_t)weekday;
    r[5] = to_bcd(m);
    r[6] = to_bcd((int)(y % 100));
}

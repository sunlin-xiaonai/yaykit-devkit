/*
 * The PCF85063A's time registers (0x04 seconds .. 0x0A years, BCD) and Unix
 * time, both ways. Plain C (no libc time zone), host-tested in
 * test/test_board.c. The clock keeps UTC.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RTC_REG_CONTROL1 0x00
#define RTC_REG_SECONDS  0x04
#define RTC_TIME_REGS    7          /* seconds, minutes, hours, days, weekdays, months, years */
#define RTC_STOP         0x20       /* Control_1: the clock is halted */
#define RTC_OS           0x80       /* Seconds: the oscillator stopped since the time was set */

/* False if the registers do not hold a time we can trust (oscillator stopped,
 * nonsense values, before 2026). */
bool rtc_regs_to_unix(const uint8_t r[RTC_TIME_REGS], int64_t *unix_s);
/* 2000..2099 only (two-digit years). */
void rtc_regs_from_unix(int64_t unix_s, uint8_t r[RTC_TIME_REGS]);

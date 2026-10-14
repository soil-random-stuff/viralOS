#include <stdbool.h>

#include "drivers/rtc.h"
#include "arch/x86_64/cpu.h"
#include "drivers/timer.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define REG_SECONDS  0x00
#define REG_MINUTES  0x02
#define REG_HOURS    0x04
#define REG_DAY      0x07
#define REG_MONTH    0x08
#define REG_YEAR     0x09
#define REG_STATUS_A 0x0A
#define REG_STATUS_B 0x0B

#define STATUS_A_UPDATING 0x80
#define STATUS_B_24HOUR   0x02
#define STATUS_B_BINARY   0x04

static uint64_t boot_seconds;   /* wall clock at uptime 0 */

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, reg | 0x80);   /* bit 7 keeps NMIs disabled, as firmware leaves them */
    return inb(CMOS_DATA);
}

struct rtc_fields {
    uint8_t second, minute, hour, day, month, year;
};

static void read_fields(struct rtc_fields *f) {
    /* Wait out an update in progress (the fields change during it). */
    for (int i = 0; i < 100000 && (cmos_read(REG_STATUS_A) & STATUS_A_UPDATING); i++) {
    }
    f->second = cmos_read(REG_SECONDS);
    f->minute = cmos_read(REG_MINUTES);
    f->hour = cmos_read(REG_HOURS);
    f->day = cmos_read(REG_DAY);
    f->month = cmos_read(REG_MONTH);
    f->year = cmos_read(REG_YEAR);
}

static bool same(const struct rtc_fields *a, const struct rtc_fields *b) {
    return a->second == b->second && a->minute == b->minute && a->hour == b->hour &&
           a->day == b->day && a->month == b->month && a->year == b->year;
}

static uint8_t from_bcd(uint8_t v) {
    return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

/* Days from 1970-01-01 to the given date (proleptic Gregorian). */
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void rtc_init(void) {
    /* Read until two reads in a row agree: then no update came in between. */
    struct rtc_fields a, b;
    read_fields(&b);
    do {
        a = b;
        read_fields(&b);
    } while (!same(&a, &b));

    uint8_t status = cmos_read(REG_STATUS_B);
    bool pm = !(status & STATUS_B_24HOUR) && (a.hour & 0x80);
    a.hour &= 0x7F;
    if (!(status & STATUS_B_BINARY)) {
        a.second = from_bcd(a.second);
        a.minute = from_bcd(a.minute);
        a.hour = from_bcd(a.hour);
        a.day = from_bcd(a.day);
        a.month = from_bcd(a.month);
        a.year = from_bcd(a.year);
    }
    if (!(status & STATUS_B_24HOUR)) {
        a.hour = (uint8_t)(a.hour % 12 + (pm ? 12 : 0));   /* 12 AM = 0, 12 PM = 12 */
    }
    if (a.month < 1 || a.month > 12 || a.day < 1 || a.day > 31) {
        return;   /* nonsense: no clock */
    }
    int64_t days = days_from_civil(2000 + a.year, a.month, a.day);
    uint64_t now = (uint64_t)(days * 86400 + a.hour * 3600 + a.minute * 60 + a.second);
    boot_seconds = now - uptime_ms() / 1000;
}

uint64_t rtc_time(void) {
    return boot_seconds + uptime_ms() / 1000;
}

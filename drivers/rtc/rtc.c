#include "rtc.h"
#include "timer.h"

/* ------------------------------------------------------------------ */
/* CMOS I/O ports                                                       */
/* ------------------------------------------------------------------ */
#define RTC_IDX   0x70U
#define RTC_DATA  0x71U

/* CMOS register addresses */
#define CMOS_SEC      0x00U
#define CMOS_MIN      0x02U
#define CMOS_HOUR     0x04U
#define CMOS_DAY      0x07U
#define CMOS_MONTH    0x08U
#define CMOS_YEAR     0x09U
#define CMOS_STATUS_A 0x0AU   /* bit 7: update-in-progress flag        */
#define CMOS_STATUS_B 0x0BU   /* bit 2: 1=binary, 0=BCD; bit 1: 1=24h */
#define CMOS_CENTURY  0x32U   /* not guaranteed, but widely present    */

/* Snapshot taken at rtc_init() time */
static unsigned int rtc_boot_epoch     = 0U;
static unsigned int rtc_boot_uptime_ms = 0U;

/* ------------------------------------------------------------------ */
/* Port helpers                                                         */
/* ------------------------------------------------------------------ */
static inline void rtc_outb(unsigned short port, unsigned char val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline unsigned char rtc_inb(unsigned short port) {
    unsigned char v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* Disable NMI while reading to avoid mid-update garbage */
static unsigned char cmos_read(unsigned char reg) {
    rtc_outb((unsigned short)RTC_IDX, (unsigned char)(reg | 0x80U));
    return rtc_inb((unsigned short)RTC_DATA);
}

static int rtc_update_in_progress(void) {
    rtc_outb((unsigned short)RTC_IDX,
             (unsigned char)(CMOS_STATUS_A | 0x80U));
    return (rtc_inb((unsigned short)RTC_DATA) & 0x80U) != 0U;
}

/* ------------------------------------------------------------------ */
/* BCD helper                                                           */
/* ------------------------------------------------------------------ */
static unsigned char bcd2bin(unsigned char bcd) {
    return (unsigned char)(((unsigned char)(bcd >> 4U) * 10U)
                           + (bcd & 0x0FU));
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void rtc_read(rtc_time_t* t) {
    unsigned char status_b;
    unsigned char sec, min, hour, day, month, year, century;
    int is_bcd;
    unsigned int full_year;

    /* Wait until no RTC update is in progress */
    while (rtc_update_in_progress()) {}

    /* Read all registers; retry once if an update started mid-read */
    do {
        sec     = cmos_read(CMOS_SEC);
        min     = cmos_read(CMOS_MIN);
        hour    = cmos_read(CMOS_HOUR);
        day     = cmos_read(CMOS_DAY);
        month   = cmos_read(CMOS_MONTH);
        year    = cmos_read(CMOS_YEAR);
        century = cmos_read(CMOS_CENTURY);
    } while (rtc_update_in_progress());

    status_b = cmos_read(CMOS_STATUS_B);
    is_bcd   = !(status_b & 0x04U);  /* bit 2 = 0 → BCD encoding */

    if (is_bcd) {
        sec     = bcd2bin(sec);
        min     = bcd2bin(min);
        hour    = bcd2bin((unsigned char)(hour & 0x7FU));
        day     = bcd2bin(day);
        month   = bcd2bin(month);
        year    = bcd2bin(year);
        century = bcd2bin(century);
    }

    /* Handle 12-hour format: bit 7 of hour byte means PM */
    if (!(status_b & 0x02U) && (hour & 0x80U)) {
        hour = (unsigned char)((hour & 0x7FU) + 12U);
    }

    /* Build full four-digit year */
    if (century != 0U) {
        full_year = (unsigned int)century * 100U + (unsigned int)year;
    } else {
        /* Assume 2000s if year < 70, else 1900s */
        full_year = ((unsigned int)year < 70U)
                    ? (2000U + (unsigned int)year)
                    : (1900U + (unsigned int)year);
    }

    t->sec   = (unsigned int)sec;
    t->min   = (unsigned int)min;
    t->hour  = (unsigned int)hour;
    t->day   = (unsigned int)day;
    t->month = (unsigned int)month;
    t->year  = full_year;
}

/* Days per month in a non-leap year */
static const unsigned int s_month_days[12] = {
    31U, 28U, 31U, 30U, 31U, 30U,
    31U, 31U, 30U, 31U, 30U, 31U
};

static int rtc_is_leap(unsigned int y) {
    return (y % 4U == 0U) && ((y % 100U != 0U) || (y % 400U == 0U));
}

unsigned int rtc_to_epoch(const rtc_time_t* t) {
    unsigned int days = 0U;
    unsigned int y, m;

    /* Accumulate full years from 1970 up to (but not including) t->year */
    for (y = 1970U; y < t->year; y++) {
        days += rtc_is_leap(y) ? 366U : 365U;
    }

    /* Accumulate full months of the current year */
    for (m = 1U; m < t->month; m++) {
        days += s_month_days[m - 1U];
        if (m == 2U && rtc_is_leap(t->year)) {
            days++;
        }
    }

    /* Add remaining days (1-based) */
    if (t->day > 0U) {
        days += t->day - 1U;
    }

    return days  * 86400U
         + t->hour * 3600U
         + t->min  *   60U
         + t->sec;
}

void rtc_init(void) {
    rtc_time_t t;
    rtc_read(&t);
    rtc_boot_uptime_ms = timer_get_uptime_ms();
    rtc_boot_epoch     = rtc_to_epoch(&t);
}

unsigned int rtc_get_wall_epoch(void) {
    unsigned int elapsed_ms = timer_get_uptime_ms() - rtc_boot_uptime_ms;
    return rtc_boot_epoch + elapsed_ms / 1000U;
}

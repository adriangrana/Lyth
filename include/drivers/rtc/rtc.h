#ifndef RTC_H
#define RTC_H

/*
 * RTC – CMOS real-time clock driver
 *
 * Reads calendar time from the CMOS RTC (ports 0x70/0x71).
 * Supports BCD and binary CMOS modes, 12/24h formats and the
 * century register (0x32).
 *
 * Used at boot to anchor wall-clock time; after that the PIT tick
 * counter keeps time monotonically without further CMOS reads.
 */

typedef struct {
    unsigned int sec;    /* 0-59  */
    unsigned int min;    /* 0-59  */
    unsigned int hour;   /* 0-23  */
    unsigned int day;    /* 1-31  */
    unsigned int month;  /* 1-12  */
    unsigned int year;   /* e.g. 2026 */
} rtc_time_t;

/* Initialise the RTC driver.  Reads CMOS once at boot and records
   the wall-clock epoch together with the current PIT uptime so that
   subsequent calls to rtc_get_wall_epoch() stay monotonic. */
void rtc_init(void);

/* Fill *t with the current CMOS real-time clock value.
   Retries automatically if an RTC update is in progress. */
void rtc_read(rtc_time_t* t);

/* Convert an rtc_time_t to an approximate Unix epoch (seconds since
   1970-01-01 00:00:00 UTC).  Accurate for dates in 1970-2099. */
unsigned int rtc_to_epoch(const rtc_time_t* t);

/* Monotonically non-decreasing wall-clock epoch:
   boot-time RTC epoch + PIT elapsed seconds.
   Never jumps backwards even if the CMOS is adjusted. */
unsigned int rtc_get_wall_epoch(void);

#endif /* RTC_H */

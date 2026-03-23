#include "timer.h"
#include "task.h"
#include "hpet.h"

static volatile unsigned int timer_ticks = 0;
static unsigned int timer_frequency = 100;

/* 64÷32 → 32 division using x86 div instruction */
static uint32_t div64_32(uint64_t dividend, uint32_t divisor) {
    uint32_t hi = (uint32_t)(dividend >> 32);
    uint32_t lo = (uint32_t)dividend;
    uint32_t q_lo, r;

    if (hi == 0)
        return lo / divisor;

    r = hi % divisor;
    __asm__ volatile ("divl %2"
        : "=a"(q_lo), "=d"(r)
        : "r"(divisor), "a"(lo), "d"(r));
    return q_lo;
}

static inline void outb(unsigned short port, unsigned char value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

void timer_init(unsigned int frequency_hz) {
    unsigned int divisor;

    if (frequency_hz == 0) {
        frequency_hz = 100;
    }

    timer_frequency = frequency_hz;
    divisor = 1193180U / frequency_hz;

    outb(0x43, 0x36);
    outb(0x40, (unsigned char)(divisor & 0xFF));
    outb(0x40, (unsigned char)((divisor >> 8) & 0xFF));
}

void timer_handle_interrupt(void) {
    timer_ticks++;
    task_on_tick();
}

unsigned int timer_get_ticks(void) {
    return timer_ticks;
}

unsigned int timer_get_frequency(void) {
    return timer_frequency;
}

unsigned int timer_ticks_to_ms(unsigned int ticks) {
    if (timer_frequency == 0) {
        return 0;
    }

    return (ticks * 1000U) / timer_frequency;
}

unsigned int timer_get_uptime_ms(void) {
    return timer_ticks_to_ms(timer_ticks);
}

unsigned int timer_get_monotonic_us(void) {
    /* If HPET is available, use it for sub-tick precision.
       HPET counter at ~14 MHz gives ~70 ns resolution.
       We combine PIT-based seconds with HPET sub-tick offset. */
    if (hpet_is_available()) {
        uint32_t freq = hpet_get_frequency();
        if (freq > 0) {
            uint32_t counter = hpet_read_counter();
            /* counter / freq = seconds.  We want microseconds.
               Split to avoid overflow: us = (counter / freq) * 10^6
                                            + ((counter % freq) * 10^6) / freq */
            uint32_t sec = counter / freq;
            uint32_t rem = counter % freq;
            return sec * 1000000U + div64_32((uint64_t)rem * 1000000ULL, freq);
        }
    }

    /* Fallback: PIT ticks */
    if (timer_frequency == 0U) {
        return 0U;
    }
    return timer_ticks * (1000000U / timer_frequency);
}

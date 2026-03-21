#include "timer.h"
#include "task.h"

static volatile unsigned int timer_ticks = 0;
static unsigned int timer_frequency = 100;

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
    /* Each PIT tick at 100 Hz = 10 ms = 10 000 µs.
       At 100 Hz this wraps after ~11.9 hours; sufficient for an
       educational kernel.  No 64-bit needed given current uptime. */
    if (timer_frequency == 0U) {
        return 0U;
    }
    return timer_ticks * (1000000U / timer_frequency);
}

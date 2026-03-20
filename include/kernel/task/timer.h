#ifndef TIMER_H
#define TIMER_H

void timer_init(unsigned int frequency_hz);
void timer_handle_interrupt(void);
unsigned int timer_get_ticks(void);
unsigned int timer_get_frequency(void);
unsigned int timer_ticks_to_ms(unsigned int ticks);
unsigned int timer_get_uptime_ms(void);

#endif

#ifndef HPET_H
#define HPET_H

#include <stdint.h>

/* HPET MMIO register offsets */
#define HPET_REG_CAP         0x000  /* General Capabilities and ID */
#define HPET_REG_CONFIG      0x010  /* General Configuration */
#define HPET_REG_STATUS      0x020  /* General Interrupt Status */
#define HPET_REG_COUNTER     0x0F0  /* Main Counter Value */

/* Timer N registers (N = 0..2) */
#define HPET_TIMER_CONFIG(n) (0x100 + 0x20 * (n))
#define HPET_TIMER_COMP(n)   (0x108 + 0x20 * (n))

/* General Configuration bits */
#define HPET_CFG_ENABLE      (1U << 0)  /* Overall enable */
#define HPET_CFG_LEGACY      (1U << 1)  /* Legacy replacement mapping */

/* Timer Configuration bits */
#define HPET_TN_INT_ENB      (1U << 2)  /* Interrupt enable */
#define HPET_TN_PERIODIC     (1U << 3)  /* Periodic mode */
#define HPET_TN_PER_CAP      (1U << 4)  /* Periodic capable (RO) */
#define HPET_TN_VAL_SET      (1U << 6)  /* Set accumulator */
#define HPET_TN_32BIT        (1U << 8)  /* Force 32-bit mode */

/*
 * hpet_init  –  Detect, map, and start the HPET main counter.
 *               Returns 1 on success, 0 if HPET not found.
 */
int  hpet_init(void);

/*
 * hpet_is_available  –  Returns 1 after a successful hpet_init().
 */
int  hpet_is_available(void);

/*
 * hpet_read_counter  –  Read the 32-bit main counter value.
 *                        (lower 32 bits of the 64-bit counter)
 */
uint32_t hpet_read_counter(void);

/*
 * hpet_period_fs  –  Counter tick period in femtoseconds.
 *                     Divide 10^15 by this to get frequency in Hz.
 */
uint32_t hpet_period_fs(void);

/*
 * hpet_get_frequency  –  Counter frequency in Hz.
 */
uint32_t hpet_get_frequency(void);

#endif

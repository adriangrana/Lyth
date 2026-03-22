#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

typedef struct {
	volatile uint32_t lock;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spinlock_acquire(spinlock_t *sl) {
	while (__sync_lock_test_and_set(&sl->lock, 1)) {
		while (sl->lock)
			__asm__ volatile ("pause" ::: "memory");
	}
}

static inline void spinlock_release(spinlock_t *sl) {
	__sync_lock_release(&sl->lock);
}

/* IRQ-safe variants: disable interrupts while holding the lock */
static inline uint32_t spinlock_acquire_irqsave(spinlock_t *sl) {
	uint32_t flags;
	__asm__ volatile ("pushf; pop %0; cli" : "=r"(flags) : : "memory");
	spinlock_acquire(sl);
	return flags;
}

static inline void spinlock_release_irqrestore(spinlock_t *sl, uint32_t flags) {
	spinlock_release(sl);
	if (flags & 0x200)
		__asm__ volatile ("sti" ::: "memory");
}

#endif

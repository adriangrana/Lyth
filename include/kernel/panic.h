#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>

/* 64-bit interrupt frame.
 * Must match the push order in interrupts64.s:
 *   SAVE_ALL pushes: rax rcx rdx rbx rbp rsi rdi r8..r15
 *   Then: vector, error_code, rip, cs, rflags, rsp, ss (CPU-pushed)
 */
typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rax;
    uint64_t vector;
    uint64_t error_code;
    /* CPU-pushed (always present in 64-bit mode) */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed)) panic_frame_t;

void panic_halt(void);
void panic_show(const char* reason, const panic_frame_t* frame, uint64_t cr2);
void panic_assert_fail(const char* expr, const char* file, int line, const char* func);
void panic_assert_fail_msg(const char* expr, const char* msg, const char* file, int line, const char* func);

#endif

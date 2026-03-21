#ifndef PANIC_H
#define PANIC_H

#include <stdint.h>

typedef struct {
    unsigned int edi;
    unsigned int esi;
    unsigned int ebp;
    unsigned int esp;
    unsigned int ebx;
    unsigned int edx;
    unsigned int ecx;
    unsigned int eax;
    unsigned int vector;
    unsigned int error_code;
    unsigned int eip;
    unsigned int cs;
    unsigned int eflags;
} panic_frame_t;

void panic_halt(void);
void panic_show(const char* reason, const panic_frame_t* frame, unsigned int cr2);
void panic_assert_fail(const char* expr, const char* file, int line, const char* func);
void panic_assert_fail_msg(const char* expr, const char* msg, const char* file, int line, const char* func);

#endif

#include "panic.h"
#include "terminal.h"
#include "serial.h"
#include "klog.h"

extern unsigned int __kernel_start;
extern unsigned int __kernel_end;

static int panic_reentry = 0;

static void panic_print_kv_hex(const char* key, unsigned int value) {
    terminal_print(key);
    terminal_print(": ");
    terminal_print_hex(value);
    terminal_put_char('\n');

    serial_print(key);
    serial_print(": ");
    serial_print_hex(value);
    serial_putc('\n');
}

static void panic_print_line(const char* s) {
    terminal_print_line(s);
    serial_print(s);
    serial_putc('\n');
}

static int panic_ptr_probably_safe(unsigned int p) {
    if ((p & 0x3U) != 0U) return 0;
    if (p < (unsigned int)(uintptr_t)&__kernel_start) return 0;
    if (p >= 0x00400000U) return 0; /* identity-mapped first 4 MiB */
    return 1;
}

static void panic_backtrace(unsigned int ebp_start, unsigned int max_frames) {
    unsigned int* frame;
    unsigned int i;

    panic_print_line("Backtrace:");

    if (!panic_ptr_probably_safe(ebp_start)) {
        panic_print_line("  <ebp no valido>");
        return;
    }

    frame = (unsigned int*)(uintptr_t)ebp_start;

    for (i = 0; i < max_frames; i++) {
        unsigned int next;
        unsigned int ret;

        if (!panic_ptr_probably_safe((unsigned int)(uintptr_t)frame)) {
            break;
        }

        next = frame[0];
        ret  = frame[1];

        terminal_print("  #");
        terminal_print_uint(i);
        terminal_print("  ");
        terminal_print_hex(ret);
        if (ret >= (unsigned int)(uintptr_t)&__kernel_start &&
            ret <= (unsigned int)(uintptr_t)&__kernel_end) {
            terminal_print("  [kernel]");
        }
        terminal_put_char('\n');

        serial_print("  #");
        serial_print_uint(i);
        serial_print("  ");
        serial_print_hex(ret);
        if (ret >= (unsigned int)(uintptr_t)&__kernel_start &&
            ret <= (unsigned int)(uintptr_t)&__kernel_end) {
            serial_print("  [kernel]");
        }
        serial_putc('\n');

        if (!panic_ptr_probably_safe(next) || next <= (unsigned int)(uintptr_t)frame) {
            break;
        }

        frame = (unsigned int*)(uintptr_t)next;
    }
}

void panic_halt(void) {
    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void panic_show(const char* reason, const panic_frame_t* frame, unsigned int cr2) {
    if (panic_reentry) {
        panic_halt();
    }
    panic_reentry = 1;

    terminal_set_color(0x4F); /* white on red */
    terminal_clear();

    panic_print_line("==================== KERNEL PANIC ====================");
    if (reason != 0) {
        terminal_print("Reason: ");
        terminal_print_line(reason);
        serial_print("Reason: ");
        serial_print(reason);
        serial_putc('\n');
    }

    if (frame != 0) {
        panic_print_kv_hex("Vector", frame->vector & 0xFFU);
        panic_print_kv_hex("Error", frame->error_code);
        panic_print_kv_hex("EIP", frame->eip);
        panic_print_kv_hex("CS", frame->cs);
        panic_print_kv_hex("EFLAGS", frame->eflags);
        if ((frame->vector & 0xFFU) == 14U) {
            panic_print_kv_hex("CR2", cr2);
        }

        panic_print_kv_hex("EAX", frame->eax);
        panic_print_kv_hex("EBX", frame->ebx);
        panic_print_kv_hex("ECX", frame->ecx);
        panic_print_kv_hex("EDX", frame->edx);
        panic_print_kv_hex("ESI", frame->esi);
        panic_print_kv_hex("EDI", frame->edi);
        panic_print_kv_hex("EBP", frame->ebp);
        panic_print_kv_hex("ESP", frame->esp);

        panic_backtrace(frame->ebp, 16U);
    }

    panic_print_line("\nKernel log:");
    klog_dump_to_terminal();

    panic_print_line("\nSystem halted.");
    panic_halt();
}

void panic_assert_fail(const char* expr, const char* file, int line, const char* func) {
    serial_print("ASSERT FAIL: ");
    serial_print(expr ? expr : "<null>");
    serial_print(" @ ");
    serial_print(file ? file : "<file>");
    serial_print(":");
    serial_print_int(line);
    serial_print(" in ");
    serial_print(func ? func : "<func>");
    serial_putc('\n');

    terminal_set_color(0x4F);
    terminal_clear();
    terminal_print_line("================ ASSERTION FAILED ================");
    terminal_print("Expr : ");
    terminal_print_line(expr ? expr : "<null>");
    terminal_print("File : ");
    terminal_print(file ? file : "<file>");
    terminal_print(":");
    terminal_print_uint((unsigned int)line);
    terminal_put_char('\n');
    terminal_print("Func : ");
    terminal_print_line(func ? func : "<func>");

    {
        unsigned int ebp;
        __asm__ volatile ("mov %%ebp, %0" : "=r"(ebp));
        panic_backtrace(ebp, 16U);
    }

    panic_halt();
}

void panic_assert_fail_msg(const char* expr, const char* msg, const char* file, int line, const char* func) {
    serial_print("ASSERT FAIL: ");
    serial_print(expr ? expr : "<null>");
    serial_print(" :: ");
    serial_print(msg ? msg : "<no-msg>");
    serial_print(" @ ");
    serial_print(file ? file : "<file>");
    serial_print(":");
    serial_print_int(line);
    serial_print(" in ");
    serial_print(func ? func : "<func>");
    serial_putc('\n');

    terminal_set_color(0x4F);
    terminal_clear();
    terminal_print_line("================ ASSERTION FAILED ================");
    terminal_print("Expr : ");
    terminal_print_line(expr ? expr : "<null>");
    terminal_print("Msg  : ");
    terminal_print_line(msg ? msg : "<no-msg>");
    terminal_print("File : ");
    terminal_print(file ? file : "<file>");
    terminal_print(":");
    terminal_print_uint((unsigned int)line);
    terminal_put_char('\n');
    terminal_print("Func : ");
    terminal_print_line(func ? func : "<func>");

    {
        unsigned int ebp;
        __asm__ volatile ("mov %%ebp, %0" : "=r"(ebp));
        panic_backtrace(ebp, 16U);
    }

    panic_halt();
}

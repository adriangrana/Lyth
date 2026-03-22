#include "panic.h"
#include "terminal.h"
#include "serial.h"
#include "klog.h"

extern unsigned int __kernel_start;
extern unsigned int __kernel_end;

static int panic_reentry = 0;

/* Colores VGA */
#define PANIC_COLOR_NORMAL   0x07  /* light gray on black */
#define PANIC_COLOR_TITLE    0x0F  /* bright white on black */
#define PANIC_COLOR_ALERT    0x0C  /* light red on black */
#define PANIC_COLOR_DIM      0x08  /* dark gray on black */
#define PANIC_COLOR_INFO     0x07  /* light gray on black */

/* ------------------------------------------------------------
 *  Helpers de salida dual: terminal + serial
 * ------------------------------------------------------------ */

static void panic_term_serial_nl(void) {
    terminal_put_char('\n');
    serial_putc('\n');
}

static void panic_puts_both(const char* s) {
    terminal_print(s);
    serial_print(s);
}

static void panic_putln_both(const char* s) {
    terminal_print_line(s);
    serial_print(s);
    serial_putc('\n');
}

static void panic_print_hex_both(unsigned int value) {
    terminal_print_hex(value);
    serial_print_hex(value);
}

static void panic_print_uint_both(unsigned int value) {
    terminal_print_uint(value);
    serial_print_uint(value);
}

static void panic_print_label_hex_both(const char* label, unsigned int value) {
    panic_puts_both(label);
    panic_print_hex_both(value);
    panic_term_serial_nl();
}

static void panic_print_pair_hex_both(const char* left_label, unsigned int left_value,
                                      const char* right_label, unsigned int right_value) {
    panic_puts_both("  ");
    panic_puts_both(left_label);
    panic_puts_both(": ");
    panic_print_hex_both(left_value);

    panic_puts_both("  ");
    panic_puts_both(right_label);
    panic_puts_both(": ");
    panic_print_hex_both(right_value);

    panic_term_serial_nl();
}

static void panic_separator(void) {
    terminal_set_color(PANIC_COLOR_DIM);
    terminal_print_line("------------------------------------------------------------");
    serial_print("------------------------------------------------------------");
    serial_putc('\n');
    terminal_set_color(PANIC_COLOR_NORMAL);
}

/* ------------------------------------------------------------
 *  Utilidades
 * ------------------------------------------------------------ */

static int panic_ptr_probably_safe(unsigned int p) {
    if ((p & 0x3U) != 0U) return 0;
    if (p < (unsigned int)(uintptr_t)&__kernel_start) return 0;
    if (p >= 0x00400000U) return 0; /* identity-mapped first 4 MiB */
    return 1;
}

static const char* panic_vector_name(unsigned int vector) {
    switch (vector & 0xFFU) {
        case 0U:  return "divide error";
        case 1U:  return "debug";
        case 2U:  return "non-maskable interrupt";
        case 3U:  return "breakpoint";
        case 4U:  return "overflow";
        case 5U:  return "bound range exceeded";
        case 6U:  return "invalid opcode";
        case 7U:  return "device not available";
        case 8U:  return "double fault";
        case 10U: return "invalid TSS";
        case 11U: return "segment not present";
        case 12U: return "stack-segment fault";
        case 13U: return "general protection fault";
        case 14U: return "page fault";
        case 16U: return "x87 floating-point exception";
        case 17U: return "alignment check";
        case 18U: return "machine check";
        case 19U: return "SIMD floating-point exception";
        default:  return "unknown exception";
    }
}

static void panic_print_banner(const char* title) {
    terminal_set_color(PANIC_COLOR_TITLE);
    panic_putln_both(title);
    terminal_set_color(PANIC_COLOR_NORMAL);
}

static void panic_print_section(const char* name) {
    terminal_set_color(PANIC_COLOR_TITLE);
    panic_putln_both(name);
    terminal_set_color(PANIC_COLOR_NORMAL);
}

static void panic_print_exception_summary(const char* reason,
                                          const panic_frame_t* frame,
                                          unsigned int cr2) {
    unsigned int vector;

    if (frame == 0) {
        terminal_set_color(PANIC_COLOR_ALERT);
        panic_puts_both("PANIC");
        if (reason != 0) {
            panic_puts_both(": ");
            panic_puts_both(reason);
        }
        panic_term_serial_nl();
        terminal_set_color(PANIC_COLOR_NORMAL);
        return;
    }

    vector = frame->vector & 0xFFU;

    terminal_set_color(PANIC_COLOR_ALERT);
    panic_puts_both("PANIC: ");
    panic_puts_both(panic_vector_name(vector));
    panic_puts_both(" at ");
    panic_print_hex_both(frame->eip);
    panic_term_serial_nl();
    terminal_set_color(PANIC_COLOR_NORMAL);

    if (reason != 0) {
        panic_puts_both("Reason: ");
        panic_puts_both(reason);
        panic_term_serial_nl();
    }

    panic_puts_both("Vector: ");
    panic_print_hex_both(vector);
    panic_puts_both("  Error: ");
    panic_print_hex_both(frame->error_code);
    panic_term_serial_nl();

    panic_puts_both("EIP:    ");
    panic_print_hex_both(frame->eip);
    panic_puts_both("  CS:    ");
    panic_print_hex_both(frame->cs);
    panic_puts_both("  EFLAGS: ");
    panic_print_hex_both(frame->eflags);
    panic_term_serial_nl();

    if (vector == 14U) {
        panic_puts_both("CR2:    ");
        panic_print_hex_both(cr2);
        panic_term_serial_nl();
    }
}

static void panic_print_registers(const panic_frame_t* frame) {
    if (frame == 0) return;

    panic_print_section("Registers:");
    panic_print_pair_hex_both("EAX", frame->eax, "EBX", frame->ebx);
    panic_print_pair_hex_both("ECX", frame->ecx, "EDX", frame->edx);
    panic_print_pair_hex_both("ESI", frame->esi, "EDI", frame->edi);
    panic_print_pair_hex_both("EBP", frame->ebp, "ESP", frame->esp);
}

static void panic_backtrace(unsigned int ebp_start, unsigned int max_frames) {
    unsigned int* frame;
    unsigned int i;

    panic_print_section("Call trace:");

    if (!panic_ptr_probably_safe(ebp_start)) {
        panic_putln_both("  <invalid frame pointer>");
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

        panic_puts_both("  #");
        panic_print_uint_both(i);
        panic_puts_both("  ");
        panic_print_hex_both(ret);

        if (ret >= (unsigned int)(uintptr_t)&__kernel_start &&
            ret <= (unsigned int)(uintptr_t)&__kernel_end) {
            panic_puts_both("  [kernel]");
        }

        panic_term_serial_nl();

        if (!panic_ptr_probably_safe(next) || next <= (unsigned int)(uintptr_t)frame) {
            break;
        }

        frame = (unsigned int*)(uintptr_t)next;
    }
}

static void panic_print_assert_common(const char* title,
                                      const char* expr,
                                      const char* msg,
                                      const char* file,
                                      int line,
                                      const char* func) {
    unsigned int ebp;

    terminal_set_color(PANIC_COLOR_NORMAL);
    terminal_clear();

    panic_print_banner(title);
    panic_putln_both("");

    terminal_set_color(PANIC_COLOR_ALERT);
    panic_putln_both("PANIC: assertion failure");
    terminal_set_color(PANIC_COLOR_NORMAL);

    panic_puts_both("Expression: ");
    panic_putln_both(expr ? expr : "<null>");

    if (msg != 0) {
        panic_puts_both("Message:    ");
        panic_putln_both(msg);
    }

    panic_puts_both("File:       ");
    panic_puts_both(file ? file : "<file>");
    panic_puts_both(":");
    terminal_print_uint((unsigned int)line);
    serial_print_int(line);
    panic_term_serial_nl();

    panic_puts_both("Function:   ");
    panic_putln_both(func ? func : "<func>");

    panic_putln_both("");
    panic_separator();

    __asm__ volatile ("mov %%ebp, %0" : "=r"(ebp));
    panic_backtrace(ebp, 16U);

    panic_putln_both("");
    panic_separator();

    terminal_set_color(PANIC_COLOR_ALERT);
    panic_putln_both("---[ end kernel panic ]---");
    terminal_set_color(PANIC_COLOR_TITLE);
    panic_putln_both("System halted.");
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

    terminal_set_color(PANIC_COLOR_NORMAL);
    terminal_clear();

    panic_print_banner("Lyth kernel panic");
    panic_putln_both("");

    panic_print_exception_summary(reason, frame, cr2);
    panic_putln_both("");

    if (frame != 0) {
        panic_separator();
        panic_print_registers(frame);
        panic_putln_both("");

        panic_separator();
        panic_backtrace(frame->ebp, 16U);
        panic_putln_both("");
    }

    panic_separator();
    panic_print_section("Kernel log:");
    klog_dump_to_terminal();
    panic_putln_both("");

    serial_print("[kernel log mirrored to terminal]\n");

    panic_separator();
    terminal_set_color(PANIC_COLOR_ALERT);
    panic_putln_both("---[ end kernel panic ]---");
    terminal_set_color(PANIC_COLOR_TITLE);
    panic_putln_both("System halted.");

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

    panic_reentry = 1;
    panic_print_assert_common("Lyth kernel panic", expr, 0, file, line, func);
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

    panic_reentry = 1;
    panic_print_assert_common("Lyth kernel panic", expr, msg, file, line, func);
    panic_halt();
}
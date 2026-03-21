/* ============================================================
 *  ktest.c  —  Kernel unit-test micro-framework
 *
 *  Output goes to:
 *    - The framebuffer terminal (always visible on screen)
 *    - COM1 serial port (capture with -serial stdio in QEMU)
 * ============================================================ */

#include "ktest.h"
#include "terminal.h"
#include "serial.h"

static int ktest_pass  = 0;
static int ktest_fail  = 0;
static const char* ktest_suite = "(none)";

void ktest_begin(const char* suite_name) {
    ktest_pass  = 0;
    ktest_fail  = 0;
    ktest_suite = suite_name ? suite_name : "(none)";

    terminal_print("[TEST] Suite: ");
    terminal_print_line(ktest_suite);
    serial_print("[TEST] Suite: ");
    serial_print(ktest_suite);
    serial_putc('\n');
}

void ktest_check(const char* name, int ok) {
    const char* result = ok ? "  PASS  " : "  FAIL  ";
    unsigned char saved_color = 0; /* terminal_get_color not exposed, use 0 */

    (void)saved_color;

    if (ok) {
        terminal_set_color(0x0A); /* bright green */
        ktest_pass++;
    } else {
        terminal_set_color(0x0C); /* bright red */
        ktest_fail++;
    }

    terminal_print(result);
    terminal_set_color(0x0F);
    terminal_print(name);
    terminal_put_char('\n');

    serial_print(ok ? "  PASS  " : "  FAIL  ");
    serial_print(name);
    serial_putc('\n');
}

void ktest_summary(void) {
    terminal_print("[TEST] ");
    terminal_print(ktest_suite);
    terminal_print(": ");

    if (ktest_fail == 0) {
        terminal_set_color(0x0A);
        terminal_print("ALL PASS");
    } else {
        terminal_set_color(0x0C);
        terminal_print("FAILED");
    }
    terminal_set_color(0x0F);
    terminal_print(" (");
    terminal_print_uint((unsigned int)ktest_pass);
    terminal_print("/");
    terminal_print_uint((unsigned int)(ktest_pass + ktest_fail));
    terminal_print_line(")");

    serial_print("[TEST] ");
    serial_print(ktest_suite);
    serial_print(ktest_fail == 0 ? ": ALL PASS (" : ": FAILED (");
    serial_print_int(ktest_pass);
    serial_putc('/');
    serial_print_int(ktest_pass + ktest_fail);
    serial_print(")\n");
}

void ktest_run(const char* name, int (*fn)(void)) {
    ktest_begin(name);
    if (fn) {
        int ok = fn();
        ktest_check(name, ok);
    }
    ktest_summary();
}

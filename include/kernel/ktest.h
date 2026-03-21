#ifndef KTEST_H
#define KTEST_H

/* Lightweight kernel test framework.
   Results are printed to both the framebuffer terminal and COM1 serial.
   Use QEMU -serial stdio to observe output on the host. */

/* Begin a named test suite. */
void ktest_begin(const char* suite_name);

/* Assert one condition inside the active suite.
   'name'  — short description of what is being checked.
   'ok'    — non-zero → PASS, zero → FAIL. */
void ktest_check(const char* name, int ok);

/* Print the final pass/fail summary for the active suite. */
void ktest_summary(void);

/* Convenience: begin + run + summary for a one-shot test function. */
void ktest_run(const char* name, int (*fn)(void));

#endif

#ifndef BOOT_TESTS_H
#define BOOT_TESTS_H

/* Run all boot-time kernel tests (heap, VFS, strings, stdio FDs).
   Output goes to both the framebuffer terminal and COM1 serial.
   Call from kernel_main() after all subsystems are initialised. */
void boot_tests_run(void);

#endif

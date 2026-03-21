#ifndef SERIAL_H
#define SERIAL_H

/* Minimal COM1 serial output driver for kernel debugging.
   Run QEMU with -serial stdio (or -serial file:serial.log) to capture output. */

void serial_init(void);
void serial_putc(char c);
void serial_print(const char* s);
void serial_print_int(int n);
void serial_print_uint(unsigned int n);
void serial_print_hex(unsigned int n);

#endif

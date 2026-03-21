/* ============================================================
 *  serial.c  —  Minimal COM1 (0x3F8) serial output driver
 *
 *  Usage (QEMU):
 *    qemu-system-i386 ... -serial stdio        # output on host terminal
 *    qemu-system-i386 ... -serial file:ser.log # output redirected to file
 * ============================================================ */

#include "serial.h"

#define COM1_DATA   0x3F8
#define COM1_IER    0x3F9   /* Interrupt Enable Register  */
#define COM1_FCR    0x3FA   /* FIFO Control Register      */
#define COM1_LCR    0x3FB   /* Line Control Register      */
#define COM1_MCR    0x3FC   /* Modem Control Register     */
#define COM1_LSR    0x3FD   /* Line Status Register       */
#define COM1_DLAB_LO 0x3F8  /* Baud rate LSB (when DLAB=1) */
#define COM1_DLAB_HI 0x3F9  /* Baud rate MSB (when DLAB=1) */

#define LSR_THRE    0x20    /* Transmit Holding Register Empty */

static inline void outb(unsigned short port, unsigned char val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline unsigned char inb(unsigned short port) {
    unsigned char val;
    __asm__ volatile ("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

void serial_init(void) {
    outb(COM1_IER,    0x00); /* disable interrupts               */
    outb(COM1_LCR,    0x80); /* enable DLAB                      */
    outb(COM1_DLAB_LO,0x01); /* baud divisor low  → 115200 baud  */
    outb(COM1_DLAB_HI,0x00); /* baud divisor high                */
    outb(COM1_LCR,    0x03); /* 8-N-1, clear DLAB                */
    outb(COM1_FCR,    0xC7); /* enable FIFO, clear, 14-byte thr  */
    outb(COM1_MCR,    0x0B); /* RTS+DTR+OUT2                     */
}

static void serial_wait_ready(void) {
    while (!(inb(COM1_LSR) & LSR_THRE))
        ;
}

void serial_putc(char c) {
    serial_wait_ready();
    outb(COM1_DATA, (unsigned char)c);
}

void serial_print(const char* s) {
    if (!s) return;
    while (*s) serial_putc(*s++);
}

void serial_print_int(int n) {
    char buf[16];
    int i = 0;
    if (n < 0) { serial_putc('-'); n = -n; }
    if (n == 0) { serial_putc('0'); return; }
    while (n > 0) { buf[i++] = (char)('0' + n % 10); n /= 10; }
    while (i > 0) serial_putc(buf[--i]);
}

void serial_print_uint(unsigned int n) {
    char buf[16];
    int i = 0;
    if (n == 0) { serial_putc('0'); return; }
    while (n > 0) { buf[i++] = (char)('0' + n % 10); n /= 10; }
    while (i > 0) serial_putc(buf[--i]);
}

void serial_print_hex(unsigned int n) {
    static const char hex[] = "0123456789abcdef";
    int i;
    serial_print("0x");
    for (i = 28; i >= 0; i -= 4)
        serial_putc(hex[(n >> i) & 0xF]);
}

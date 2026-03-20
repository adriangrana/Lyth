#include "interrupts.h"
#include "idt.h"
#include "keyboard.h"
#include "timer.h"
#include "syscall.h"
#include "task.h"

typedef struct {
    unsigned int edi;
    unsigned int esi;
    unsigned int ebp;
    unsigned int esp;
    unsigned int ebx;
    unsigned int edx;
    unsigned int ecx;
    unsigned int eax;
    unsigned int eip;
    unsigned int cs;
    unsigned int eflags;
} interrupt_frame_t;

static inline void outb(unsigned short port, unsigned char value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

extern void irq0_stub(void);
extern void irq1_stub(void);
extern void syscall_stub(void);

static void pic_remap(void) {
    unsigned char master_mask = 0xFF;
    unsigned char slave_mask = 0xFF;

    outb(0x20, 0x11);
    outb(0xA0, 0x11);

    outb(0x21, 0x20);
    outb(0xA1, 0x28);

    outb(0x21, 0x04);
    outb(0xA1, 0x02);

    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    (void)master_mask;
    (void)slave_mask;

    outb(0x21, 0xFC);
    outb(0xA1, 0xFF);
}

static void pic_send_eoi(unsigned char irq) {
    if (irq >= 8) {
        outb(0xA0, 0x20);
    }
    outb(0x20, 0x20);
}

unsigned int timer_interrupt_handler(unsigned int current_esp) {
    timer_handle_interrupt();
    pic_send_eoi(0);
    return task_schedule_on_timer(current_esp);
}

unsigned int keyboard_interrupt_handler(unsigned int current_esp) {
    keyboard_handle_interrupt();
    pic_send_eoi(1);
    return current_esp;
}

unsigned int syscall_interrupt_handler(unsigned int current_esp) {
    interrupt_frame_t* frame = (interrupt_frame_t*)current_esp;

    frame->eax = syscall_callback(frame->eax,
                                  frame->ebx,
                                  frame->ecx,
                                  frame->edx,
                                  frame->esi);

    return task_schedule_on_syscall(current_esp);
}

void interrupts_init(void) {
    idt_init();
    pic_remap();
    timer_init(100);

    idt_set_gate(32, (unsigned int)irq0_stub, 0x10, 0x8E);
    idt_set_gate(33, (unsigned int)irq1_stub, 0x10, 0x8E);
    idt_set_gate(0x80, (unsigned int)syscall_stub, 0x10, 0x8E);

    __asm__ volatile ("sti");
}